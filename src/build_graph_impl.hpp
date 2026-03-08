#pragma once

// Implementation of BuildGraph<NodeT> methods that depend on HashStore and
// ThreadPool.  Separated from build_graph.hpp to break the circular include
// (build_graph.hpp → hash_store.hpp → ...).  Included at the bottom of
// build_graph.hpp after all declarations are visible.

#include "hash_store.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>

template<BuildNodeType NodeT>
void BuildGraph<NodeT>::rebuild_all(ThreadPool& pool, HashStore& store) {
    auto order = topo_sort();

    // Track how many nodes have been fully finished (built or skipped).
    // We process nodes level-by-level: a node is ready once all its deps
    // have been completed.
    std::unordered_map<NodeId, std::atomic<bool>> done_flags;
    for (const auto& id : order) done_flags[id].store(false);

    std::mutex              cv_mtx;
    std::condition_variable cv;

    // Helper: are all deps of `id` done?
    auto deps_done = [&](const NodeId& id) -> bool {
        const auto& node = nodes_.at(id);
        return std::ranges::all_of(node->deps, [&](const NodeId& dep) {
            return done_flags.at(dep).load();
        });
    };

    std::atomic<int> remaining{static_cast<int>(order.size())};

    for (const auto& id : order) {
        auto& node = nodes_.at(id);

        // Wait until all dependencies are finished.
        {
            std::unique_lock lk(cv_mtx);
            cv.wait(lk, [&] { return deps_done(id); });
        }

        if (!dirty_.contains(id)) {
            // Node is clean — mark done immediately and continue.
            done_flags[id].store(true);
            cv.notify_all();
            --remaining;
            continue;
        }

        // Hash-check: compute current hash of the source file.
        const std::string current_hash = HashStore::hash_file(node->src);
        if (current_hash == store.get(node->id)) {
            // Content unchanged — skip rebuild even though flagged dirty.
            std::cout << "[skip]  " << node->id << "\n";
            done_flags[id].store(true);
            cv.notify_all();
            --remaining;
            continue;
        }

        // Dispatch build to the thread pool.
        pool.submit([&, id, current_hash]() mutable {
            auto& n = nodes_.at(id);
            std::cout << "[build] " << n->node_type() << " → " << n->dst.string() << "\n";
            try {
                n->build_action();
                store.set(n->id, current_hash);  // update hash after successful build
            } catch (const std::exception& e) {
                std::cerr << "[error] " << n->id << ": " << e.what() << "\n";
            }
            done_flags[id].store(true);
            cv.notify_all();
            --remaining;
        });
    }

    // Wait for all dispatched tasks to finish.
    pool.wait_all();

    // Clear dirty set — a fresh watch cycle will re-populate it.
    dirty_.clear();
}

template<BuildNodeType NodeT>
void BuildGraph<NodeT>::serialize_cache(HashStore& store) const {
    // The store is updated in-place during rebuild_all(); here we just flush.
    (void)store;  // flush is called by the caller via store.save()
}
