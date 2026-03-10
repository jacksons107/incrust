#pragma once

#include "build_node.hpp"

#include <algorithm>
#include <concepts>
#include <functional>
#include <iostream>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// NodeT must publicly derive from BuildNode.
template<typename T>
concept BuildNodeType = std::derived_from<T, BuildNode>;

class HashStore;
class ThreadPool;

template<BuildNodeType NodeT>
class BuildGraph {
public:
    void add_node(std::unique_ptr<NodeT> node) {
        const NodeId id = node->id;
        nodes_.emplace(id, std::move(node));
    }

    // Mark a node and all transitive dependents as needing a rebuild.
    void mark_dirty(const NodeId& id) {
        if (!nodes_.contains(id)) return;
        std::queue<NodeId> q;
        q.push(id);
        while (!q.empty()) {
            auto cur = q.front(); q.pop();
            if (dirty_.insert(cur).second) {
                for (const auto& dependent : reverse_deps_of(cur))
                    q.push(dependent);
            }
        }
    }

    void mark_all_dirty() {
        for (const auto& [id, _] : nodes_)
            dirty_.insert(id);
    }

    bool has_node(const NodeId& id) const { return nodes_.contains(id); }
    bool is_dirty(const NodeId& id)  const { return dirty_.contains(id); }

    // Returns node IDs in topological order (dependencies first).
    // Throws std::runtime_error on a cycle.
    [[nodiscard]] std::vector<NodeId> topo_sort() const {
        // Kahn's algorithm
        std::unordered_map<NodeId, int> in_degree;
        for (const auto& [id, _] : nodes_) in_degree[id] = 0;

        for (const auto& [id, node] : nodes_)
            in_degree[id] += static_cast<int>(node->deps.size());

        std::queue<NodeId> ready;
        for (const auto& [id, deg] : in_degree)
            if (deg == 0) ready.push(id);

        std::vector<NodeId> order;
        order.reserve(nodes_.size());

        while (!ready.empty()) {
            auto cur = ready.front(); ready.pop();
            order.push_back(cur);

            for (const auto& dependent : reverse_deps_of(cur)) {
                if (--in_degree[dependent] == 0)
                    ready.push(dependent);
            }
        }

        if (order.size() != nodes_.size())
            throw std::runtime_error("BuildGraph: cycle detected in dependency graph");

        return order;
    }

    // Rebuild every dirty node whose input hash has changed; independent nodes
    // at the same depth are dispatched concurrently to the thread pool.
    void rebuild_all(ThreadPool& pool, HashStore& store);

    void serialize_cache(HashStore& store) const;

    const auto& nodes() const { return nodes_; }

private:
    std::unordered_map<NodeId, std::unique_ptr<NodeT>> nodes_;
    std::unordered_set<NodeId> dirty_;

    // Reverse adjacency list; rebuilt lazily when add_node() invalidates it.
    mutable std::unordered_map<NodeId, std::vector<NodeId>> rev_deps_;
    mutable bool rev_deps_valid_{false};

    void build_rev_deps() const {
        rev_deps_.clear();
        for (const auto& [id, node] : nodes_) {
            if (!rev_deps_.contains(id)) rev_deps_[id] = {};
            for (const auto& dep : node->deps)
                rev_deps_[dep].push_back(id);
        }
        rev_deps_valid_ = true;
    }

    const std::vector<NodeId>& reverse_deps_of(const NodeId& id) const {
        if (!rev_deps_valid_) build_rev_deps();
        static const std::vector<NodeId> empty;
        auto it = rev_deps_.find(id);
        return it != rev_deps_.end() ? it->second : empty;
    }
};

// rebuild_all and serialize_cache are defined in build_graph_impl.hpp,
// included here to keep the template self-contained without a .cpp instantiation.
#include "build_graph_impl.hpp"
