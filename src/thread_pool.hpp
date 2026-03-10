#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

// Fixed-size thread pool backed by std::jthread workers (C++20).
// Destructor requests a stop and lets jthread join automatically.

class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count
                        = std::thread::hardware_concurrency())
    {
        workers_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this](std::stop_token stoken) {
                worker_loop(stoken);
            });
        }
    }

    // Queue a task and return immediately.
    void submit(std::function<void()> task) {
        {
            std::scoped_lock lk(mtx_);
            tasks_.push(std::move(task));
            ++pending_;
        }
        cv_work_.notify_one();
    }

    void wait_all() {
        std::unique_lock lk(mtx_);
        cv_done_.wait(lk, [this] { return pending_ == 0; });
    }

    ~ThreadPool() {
        for (auto& w : workers_) w.request_stop();
        cv_work_.notify_all();
    }

    std::size_t size() const { return workers_.size(); }

private:
    void worker_loop(std::stop_token stoken) {
        while (!stoken.stop_requested()) {
            std::function<void()> task;
            {
                std::unique_lock lk(mtx_);
                cv_work_.wait(lk, [&] {
                    return !tasks_.empty() || stoken.stop_requested();
                });
                if (stoken.stop_requested() && tasks_.empty()) break;
                task = std::move(tasks_.front());
                tasks_.pop();
            }

            // Execute outside the lock so other workers can pick up tasks concurrently.
            task();

            {
                std::scoped_lock lk(mtx_);
                --pending_;
            }
            cv_done_.notify_all();
        }
    }

    std::vector<std::jthread>           workers_;
    std::queue<std::function<void()>>   tasks_;
    std::mutex                          mtx_;
    std::condition_variable             cv_work_;
    std::condition_variable             cv_done_;
    int                                 pending_{0};
};
