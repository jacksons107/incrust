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

// Producer/consumer file watcher.
//
// A single std::jthread polls every tracked path's last_write_time at a
// configurable interval (default 500 ms).  When a change is detected the
// node's ID is pushed onto a shared queue.
//
// The main thread calls pop_dirty() to drain the queue and schedule rebuilds.
//
//   [jthread: poll loop] ──(mutex + cv)──► [queue<NodeId>]
//                                                ▲
//                                    main thread drain via pop_dirty()

class FileWatcher {
public:
    using Clock    = std::chrono::steady_clock;
    using Duration = std::chrono::milliseconds;

    // Register a set of (path → nodeId) pairs to watch.
    // The watcher thread is NOT started here — call start() explicitly so that
    // the graph can be fully populated before watching begins.
    FileWatcher() = default;

    // Register a single file.  May be called before or after start().
    void watch(const fs::path& path, const NodeId& id) {
        std::scoped_lock lk(mtx_);
        auto mtime = fs::exists(path) ? fs::last_write_time(path)
                                      : fs::file_time_type{};
        entries_[path.string()] = Entry{ id, mtime };
    }

    // Register many files at once.
    void watch_many(const std::vector<std::pair<fs::path, NodeId>>& files) {
        for (const auto& [p, id] : files) watch(p, id);
    }

    // Start the background polling thread.
    void start(Duration interval = Duration{500}) {
        interval_ = interval;
        watcher_thread_ = std::jthread([this](std::stop_token st) {
            poll_loop(st);
        });
    }

    // Stop the watcher (the jthread destructor will also stop it, but this
    // lets callers stop explicitly without destroying the object).
    void stop() {
        watcher_thread_.request_stop();
    }

    // Non-blocking drain: pops one dirty node ID from the queue.
    // Returns true and writes to `out` if a dirty node was available.
    bool pop_dirty(NodeId& out) {
        std::scoped_lock lk(mtx_);
        if (dirty_queue_.empty()) return false;
        out = std::move(dirty_queue_.front());
        dirty_queue_.pop();
        return true;
    }

    // Block until at least one dirty event arrives (or timeout elapses).
    // Returns true if an event is available, false on timeout.
    bool wait_for_dirty(Duration timeout = Duration{1000}) {
        std::unique_lock lk(mtx_);
        return cv_.wait_for(lk, timeout, [this] {
            return !dirty_queue_.empty();
        });
    }

    // Destructor — jthread joins automatically; no manual cleanup needed.
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
    std::unordered_map<std::string, Entry>     entries_;   // guarded by mtx_
    std::queue<NodeId>                         dirty_queue_; // guarded by mtx_
};
