#pragma once

#include "processor.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Stable identifier for a node in the build graph.
// Using the source path string as the canonical ID keeps things human-readable
// and makes cache keys trivially derivable.
using NodeId = std::string;

// ─── Abstract base ────────────────────────────────────────────────────────────

struct BuildNode {
    NodeId      id;           // unique key == src.string()
    fs::path    src;          // input path
    fs::path    dst;          // output path
    std::string cached_hash;  // last hash seen (loaded from HashStore)

    // IDs of nodes this node depends on; must be built first.
    std::vector<NodeId> deps;

    // The build action is a std::function<void()> so the graph can dispatch it
    // to a thread pool without knowing anything about the node type.
    std::function<void()> build_action;

    // Virtual destructor keeps unique_ptr<BuildNode> well-behaved.
    virtual ~BuildNode() = default;

    virtual std::string_view node_type() const = 0;

protected:
    BuildNode(NodeId id_, fs::path src_, fs::path dst_)
        : id(std::move(id_)), src(std::move(src_)), dst(std::move(dst_)) {}
};

// ─── Concrete node types ──────────────────────────────────────────────────────

// Renders a Markdown source file to HTML, optionally wrapped in a layout.
struct MarkdownNode : BuildNode {
    std::unique_ptr<Processor> renderer;  // owns a MarkdownProcessor

    MarkdownNode(NodeId id, fs::path src, fs::path dst,
                 std::unique_ptr<Processor> proc)
        : BuildNode(std::move(id), std::move(src), std::move(dst))
        , renderer(std::move(proc))
    {
        build_action = [this] {
            fs::create_directories(this->dst.parent_path());
            renderer->process(this->src, this->dst);
        };
    }

    std::string_view node_type() const override { return "MarkdownNode"; }
};

// Applies a template layout (substitutes {{content}} and other tokens).
struct TemplateNode : BuildNode {
    std::unique_ptr<Processor> engine;   // owns a TemplateProcessor

    TemplateNode(NodeId id, fs::path src, fs::path dst,
                 std::unique_ptr<Processor> proc)
        : BuildNode(std::move(id), std::move(src), std::move(dst))
        , engine(std::move(proc))
    {
        build_action = [this] {
            fs::create_directories(this->dst.parent_path());
            engine->process(this->src, this->dst);
        };
    }

    std::string_view node_type() const override { return "TemplateNode"; }
};

// Passthrough copy (or future minification) for static assets.
struct AssetNode : BuildNode {
    std::unique_ptr<Processor> copier;   // owns a CopyProcessor

    AssetNode(NodeId id, fs::path src, fs::path dst,
              std::unique_ptr<Processor> proc)
        : BuildNode(std::move(id), std::move(src), std::move(dst))
        , copier(std::move(proc))
    {
        build_action = [this] {
            fs::create_directories(this->dst.parent_path());
            copier->process(this->src, this->dst);
        };
    }

    std::string_view node_type() const override { return "AssetNode"; }
};
