#pragma once
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

// Simple thread pool using a FIFO task queue (Round Robin over tasks)
class ThreadPool {
public:
    explicit ThreadPool(size_t threadCount = std::thread::hardware_concurrency())
        : stop(false)
    {
        if (threadCount == 0) threadCount = 4;
        for (size_t i = 0; i < threadCount; ++i) {
            workers.emplace_back([this]() {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queueMutex);
                        condition.wait(lock, [this]() {
                            return stop || !tasks.empty();
                        });
                        if (stop && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    // Execute one task (a "time slice") from the queue
                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            stop = true;
        }
        condition.notify_all();
        for (auto &t : workers) {
            if (t.joinable()) t.join();
        }
    }

    void enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            tasks.push(std::move(task));
        }
        condition.notify_one();
    }

private:
    std::vector<std::thread>            workers;
    std::queue<std::function<void()>>   tasks;
    std::mutex                          queueMutex;
    std::condition_variable             condition;
    bool                                stop;
};
