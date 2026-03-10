#pragma once

// Template method bodies separated from build_graph.hpp to avoid a circular
// include through hash_store.hpp. Included at the bottom of build_graph.hpp.

#include "hash_store.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>

template<BuildNodeType NodeT>
void BuildGraph<NodeT>::rebuild_all(ThreadPool& pool, HashStore& store) {
    auto order = topo_sort();

    std::unordered_map<NodeId, std::atomic<bool>> done_flags;
    for (const auto& id : order) done_flags[id].store(false);

    std::mutex              cv_mtx;
    std::condition_variable cv;

    auto deps_done = [&](const NodeId& id) -> bool {
        const auto& node = nodes_.at(id);
        return std::ranges::all_of(node->deps, [&](const NodeId& dep) {
            return done_flags.at(dep).load();
        });
    };

    std::atomic<int> remaining{static_cast<int>(order.size())};

    for (const auto& id : order) {
        auto& node = nodes_.at(id);

        {
            std::unique_lock lk(cv_mtx);
            cv.wait(lk, [&] { return deps_done(id); });
        }

        if (!dirty_.contains(id)) {
            done_flags[id].store(true);
            cv.notify_all();
            --remaining;
            continue;
        }

        const std::string current_hash = HashStore::hash_file(node->src);
        if (current_hash == store.get(node->id)) {
            std::cout << "[skip]  " << node->id << "\n";
            done_flags[id].store(true);
            cv.notify_all();
            --remaining;
            continue;
        }

        pool.submit([&, id, current_hash]() mutable {
            auto& n = nodes_.at(id);
            std::cout << "[build] " << n->node_type() << " → " << n->dst.string() << "\n";
            try {
                n->build_action();
                store.set(n->id, current_hash);
            } catch (const std::exception& e) {
                std::cerr << "[error] " << n->id << ": " << e.what() << "\n";
            }
            done_flags[id].store(true);
            cv.notify_all();
            --remaining;
        });
    }

    pool.wait_all();
    dirty_.clear();
}

template<BuildNodeType NodeT>
void BuildGraph<NodeT>::serialize_cache(HashStore& store) const {
    // Store is updated in-place during rebuild_all(); caller flushes via store.save().
    (void)store;
}
