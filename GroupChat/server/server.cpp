#include <iostream>
#include <unordered_map>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <ctime>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "thread_pool.h"
#include "../shared/protocol.h"

struct CachedMessage {
    ChatPacket packet;
    std::chrono::steady_clock::time_point expiry;
};

// Shared state
std::unordered_map<uint16_t, std::vector<int>> groupMembers;      // groupID -> client fds
std::unordered_map<uint16_t, std::deque<CachedMessage>> groupCaches; // groupID -> recent msgs
std::unordered_map<int, uint16_t> clientGroup;                    // fd -> groupID
std::mutex stateMutex;

const size_t MAX_CACHE_PER_GROUP = 50;
const std::chrono::seconds CACHE_TTL(60);

std::atomic<bool> running(true);
std::atomic<uint64_t> totalMessages(0);
std::chrono::steady_clock::time_point startTime;

std::ofstream logFile;

// ---------- Helper functions ----------

void cleanup_client_nolock(int fd) {
    // NOTE: caller must hold stateMutex
    auto it = clientGroup.find(fd);
    if (it != clientGroup.end()) {
        uint16_t g = it->second;
        auto gIt = groupMembers.find(g);
        if (gIt != groupMembers.end()) {
            auto &vec = gIt->second;
            vec.erase(std::remove(vec.begin(), vec.end(), fd), vec.end());
        }
        clientGroup.erase(it);
    }
}

void cleanup_client(int fd) {
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        cleanup_client_nolock(fd);
    }
    close(fd);
}

void send_packet_fd(int fd, const ChatPacket &pkt) {
    ssize_t n = send(fd, &pkt, sizeof(pkt), 0);
    if (n <= 0) {
        // assume client died
        cleanup_client(fd);
    }
}

void broadcast_to_group(uint16_t groupID, const ChatPacket &pkt, int excludeFd = -1) {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto it = groupMembers.find(groupID);
    if (it == groupMembers.end()) return;

    for (int fd : it->second) {
        if (fd == excludeFd) continue;
        send_packet_fd(fd, pkt);
    }
}

void cache_message(uint16_t groupID, const ChatPacket &pkt) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(stateMutex);
    auto &dq = groupCaches[groupID];
    dq.push_back({pkt, now + CACHE_TTL});
    while (dq.size() > MAX_CACHE_PER_GROUP) dq.pop_front();
}

void send_cached_messages(int fd, uint16_t groupID) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(stateMutex);
    auto &dq = groupCaches[groupID];

    // TTL eviction
    while (!dq.empty() && dq.front().expiry < now) dq.pop_front();

    for (auto &cm : dq) {
        send_packet_fd(fd, cm.packet);
    }
}

void handle_packet(int client_fd, ChatPacket pkt) {
    // Server sets timestamp
    pkt.timestamp = static_cast<uint32_t>(std::time(nullptr));

    if (pkt.type == PKT_JOIN) {
        uint16_t groupID = pkt.groupID;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            groupMembers[groupID].push_back(client_fd);
            clientGroup[client_fd] = groupID;
        }
        std::cout << "Client " << client_fd << " joined group " << groupID << std::endl;
        send_cached_messages(client_fd, groupID);

    } else if (pkt.type == PKT_MSG) {
        uint16_t groupID;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            auto it = clientGroup.find(client_fd);
            if (it == clientGroup.end()) {
                // Ignore messages from unjoined clients
                return;
            }
            groupID = it->second;
        }

        // Log
        if (logFile.is_open()) {
            logFile << pkt.timestamp << " [group " << groupID
                    << "] fd " << client_fd << ": " << pkt.payload << std::endl;
        }

        cache_message(groupID, pkt);
        broadcast_to_group(groupID, pkt, client_fd);
        totalMessages++;

                // Simple performance print every 20 messages
        if (totalMessages % 20 == 0) {
            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(now - startTime).count();
            if (secs > 0.0) {
                double throughput = totalMessages / secs;
                std::cout << "[Perf] " << totalMessages
                          << " messages in " << secs
                          << " seconds (" << throughput << " msg/sec)"
                          << std::endl;
            }
        }


    } else if (pkt.type == PKT_LEAVE) {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            cleanup_client_nolock(client_fd);
        }
        close(client_fd);
        std::cout << "Client " << client_fd << " left." << std::endl;
    }
}

void client_reader(int client_fd, ThreadPool &pool) {
    while (running) {
        ChatPacket pkt{};
        ssize_t n = recv(client_fd, &pkt, sizeof(pkt), 0);
        if (n == 0) {
            std::cout << "Client " << client_fd << " disconnected." << std::endl;
            cleanup_client(client_fd);
            break;
        } else if (n < 0) {
            std::perror("recv");
            cleanup_client(client_fd);
            break;
        } else if (n == sizeof(pkt)) {
            // push work to thread pool
            pool.enqueue([client_fd, pkt]() mutable {
                handle_packet(client_fd, pkt);
            });
        } else {
            std::cout << "Received partial/invalid packet from " << client_fd << std::endl;
        }
    }
}

void signal_handler(int) {
    running = false;
}

int main() {
    signal(SIGINT, signal_handler);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::perror("bind");
        return 1;
    }

    if (listen(server_fd, 16) < 0) {
        std::perror("listen");
        return 1;
    }

    // Open log file
    logFile.open("logs/chat_log.txt", std::ios::app);
    if (!logFile.is_open()) {
        std::cerr << "Warning: could not open logs/chat_log.txt" << std::endl;
    }

    ThreadPool pool(4);          // 4 worker threads
    std::vector<std::thread> clientThreads;
    startTime = std::chrono::steady_clock::now();

    std::cout << "Server listening on port 8080..." << std::endl;

    while (running) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &len);
        if (client_fd < 0) {
            if (!running) break;
            std::perror("accept");
            continue;
        }
        std::cout << "New client fd " << client_fd << std::endl;
        clientThreads.emplace_back(client_reader, client_fd, std::ref(pool));
    }

    for (auto &t : clientThreads) {
        if (t.joinable()) t.join();
    }

    close(server_fd);

    auto end = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(end - startTime).count();
    std::cout << "Total messages: " << totalMessages << std::endl;
    std::cout << "Elapsed seconds: " << secs << std::endl;
    if (secs > 0) {
        std::cout << "Throughput: " << (totalMessages / secs) << " msg/sec" << std::endl;
    }

    return 0;
}
