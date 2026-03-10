#pragma once

#include "build_node.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <queue>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

// Polls tracked file mtimes on a background jthread and pushes dirty node IDs
// onto a queue. The main thread drains it via pop_dirty().

class FileWatcher {
public:
    using Clock    = std::chrono::steady_clock;
    using Duration = std::chrono::milliseconds;

    FileWatcher() = default;

    void watch(const fs::path& path, const NodeId& id) {
        std::scoped_lock lk(mtx_);
        auto mtime = fs::exists(path) ? fs::last_write_time(path)
                                      : fs::file_time_type{};
        entries_[path.string()] = Entry{ id, mtime };
    }

    void watch_many(const std::vector<std::pair<fs::path, NodeId>>& files) {
        for (const auto& [p, id] : files) watch(p, id);
    }

    void start(Duration interval = Duration{500}) {
        interval_ = interval;
        watcher_thread_ = std::jthread([this](std::stop_token st) {
            poll_loop(st);
        });
    }

    void stop() {
        watcher_thread_.request_stop();
    }

    // Pop one dirty node ID. Returns false if queue is empty.
    bool pop_dirty(NodeId& out) {
        std::scoped_lock lk(mtx_);
        if (dirty_queue_.empty()) return false;
        out = std::move(dirty_queue_.front());
        dirty_queue_.pop();
        return true;
    }

    // Block until a dirty event arrives or timeout elapses.
    bool wait_for_dirty(Duration timeout = Duration{1000}) {
        std::unique_lock lk(mtx_);
        return cv_.wait_for(lk, timeout, [this] {
            return !dirty_queue_.empty();
        });
    }

    ~FileWatcher() = default;

private:
    struct Entry {
        NodeId               node_id;
        fs::file_time_type   last_mtime;
    };

    void poll_loop(std::stop_token stoken) {
        while (!stoken.stop_requested()) {
            std::this_thread::sleep_for(interval_);

            std::scoped_lock lk(mtx_);
            for (auto& [path_str, entry] : entries_) {
                fs::path p(path_str);
                if (!fs::exists(p)) continue;

                auto mtime = fs::last_write_time(p);
                if (mtime != entry.last_mtime) {
                    entry.last_mtime = mtime;
                    dirty_queue_.push(entry.node_id);
                    cv_.notify_all();
                }
            }
        }
    }

    std::jthread                               watcher_thread_;
    Duration                                   interval_{500};

    std::mutex                                 mtx_;
    std::condition_variable                    cv_;
    std::unordered_map<std::string, Entry>     entries_;
    std::queue<NodeId>                         dirty_queue_;
};
