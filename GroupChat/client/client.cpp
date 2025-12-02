#include <iostream>
#include <thread>
#include <string>
#include <cstring>
#include <csignal>
#include <atomic>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "../shared/protocol.h"

std::atomic<bool> running(true);

void signal_handler(int) {
    running = false;
}

// Background thread: receives packets from server and prints them
void receive_loop(int sock) {
    while (running) {
        ChatPacket pkt{};
        ssize_t n = recv(sock, &pkt, sizeof(pkt), 0);
        if (n == 0) {
            std::cout << "\n[Server closed connection]\n";
            running = false;
            break;
        } else if (n < 0) {
            std::perror("recv");
            running = false;
            break;
        } else if (n == sizeof(pkt)) {
            if (pkt.type == PKT_MSG) {
                std::cout << "\n[Group " << pkt.groupID << "] "
                          << pkt.payload << "\n> " << std::flush;
            }
            // We ignore JOIN/LEAVE packets on client side for simplicity
        } else {
            // partial packet, ignore for now
        }
    }
}

int main() {
    signal(SIGINT, signal_handler);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::perror("socket");
        return 1;
    }

    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(8080);
    if (inet_pton(AF_INET, "127.0.0.1", &serv.sin_addr) <= 0) {
        std::perror("inet_pton");
        return 1;
    }

    if (connect(sock, (sockaddr*)&serv, sizeof(serv)) < 0) {
        std::perror("connect");
        return 1;
    }

    std::string username;
    uint16_t groupID;

    std::cout << "Enter username: ";
    std::getline(std::cin, username);
    if (username.empty()) username = "anon";
    if (username.size() > 50) username.resize(50);

    std::cout << "Enter group ID (e.g., 1): ";
    std::cin >> groupID;
    std::cin.ignore(); // clear newline

    // Send JOIN packet
    ChatPacket joinPkt{};
    joinPkt.type = PKT_JOIN;
    joinPkt.groupID = groupID;
    std::snprintf(joinPkt.payload, sizeof(joinPkt.payload),
                  "%s joined", username.c_str());
    send(sock, &joinPkt, sizeof(joinPkt), 0);

    std::thread recvThread(receive_loop, sock);

    std::cout << "Type messages, '/quit' to exit.\n";
    std::string line;
    while (running) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) break;
        if (line == "/quit") {
            ChatPacket leave{};
            leave.type = PKT_LEAVE;
            leave.groupID = groupID;
            send(sock, &leave, sizeof(leave), 0);
            running = false;
            break;
        }

        ChatPacket msg{};
        msg.type = PKT_MSG;
        msg.groupID = groupID;
        std::snprintf(msg.payload, sizeof(msg.payload),
                      "%s: %s", username.c_str(), line.c_str());
        send(sock, &msg, sizeof(msg), 0);
    }

    if (recvThread.joinable()) recvThread.join();
    close(sock);
    return 0;
}
