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

// ─── Concept ──────────────────────────────────────────────────────────────────
// NodeT must publicly derive from BuildNode so the graph can access the common
// fields (id, deps, build_action, cached_hash) without knowing the concrete type.

template<typename T>
concept BuildNodeType = std::derived_from<T, BuildNode>;

// ─── Forward declaration ──────────────────────────────────────────────────────
class HashStore;
class ThreadPool;

// ─── BuildGraph<NodeT> ────────────────────────────────────────────────────────

template<BuildNodeType NodeT>
class BuildGraph {
public:
    // ── Mutation ──────────────────────────────────────────────────────────────

    // Transfer ownership of a node into the graph.
    void add_node(std::unique_ptr<NodeT> node) {
        const NodeId id = node->id;
        nodes_.emplace(id, std::move(node));
    }

    // Mark a node (and all transitive dependents) as needing a rebuild.
    void mark_dirty(const NodeId& id) {
        if (!nodes_.contains(id)) return;
        // BFS over the reverse-dependency edges.
        std::queue<NodeId> q;
        q.push(id);
        while (!q.empty()) {
            auto cur = q.front(); q.pop();
            if (dirty_.insert(cur).second) {       // newly dirtied
                for (const auto& dependent : reverse_deps_of(cur))
                    q.push(dependent);
            }
        }
    }

    // Mark every node dirty (used for a clean full rebuild).
    void mark_all_dirty() {
        for (const auto& [id, _] : nodes_)
            dirty_.insert(id);
    }

    // ── Queries ───────────────────────────────────────────────────────────────

    bool has_node(const NodeId& id) const { return nodes_.contains(id); }
    bool is_dirty(const NodeId& id)  const { return dirty_.contains(id); }

    // Returns all node IDs in a valid topological order (dependencies first).
    // Throws std::runtime_error if the graph has a cycle.
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

            // Reduce in-degree of every node that depended on cur.
            for (const auto& dependent : reverse_deps_of(cur)) {
                if (--in_degree[dependent] == 0)
                    ready.push(dependent);
            }
        }

        if (order.size() != nodes_.size())
            throw std::runtime_error("BuildGraph: cycle detected in dependency graph");

        return order;
    }

    // ── Build ─────────────────────────────────────────────────────────────────

    // Walk the topological order and rebuild every dirty node whose input hash
    // has actually changed.  Independent nodes at the same depth are dispatched
    // concurrently to the thread pool.
    //
    // The pool and store parameters are taken by reference — they outlive the
    // rebuild call and are shared across the whole session.
    void rebuild_all(ThreadPool& pool, HashStore& store);

    // Persist current hashes back to the store (call after rebuild_all).
    void serialize_cache(HashStore& store) const;

    // ── Iteration ─────────────────────────────────────────────────────────────

    // Expose raw node map for read-only inspection (e.g. by FileWatcher).
    const auto& nodes() const { return nodes_; }

private:
    std::unordered_map<NodeId, std::unique_ptr<NodeT>> nodes_;
    std::unordered_set<NodeId> dirty_;

    // Lazily-built reverse adjacency list: dependent → set of its dependencies.
    // Rebuilt on demand because add_node() invalidates it.
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

// ── rebuild_all and serialize_cache are defined in build_graph_impl.hpp ───────
// (Included below to keep this header self-contained while avoiding a .cpp
//  translation unit that would need to explicitly instantiate the template.)
#include "build_graph_impl.hpp"
