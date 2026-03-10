#pragma once

#include "processor.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Stable identifier for a node; uses the source path string as the canonical key.
using NodeId = std::string;

struct BuildNode {
    NodeId      id;
    fs::path    src;
    fs::path    dst;
    std::string cached_hash;

    // IDs of nodes this node depends on; must be built first.
    std::vector<NodeId> deps;

    // Callable dispatched to the thread pool; set by each concrete subclass.
    std::function<void()> build_action;

    virtual ~BuildNode() = default;

    virtual std::string_view node_type() const = 0;

protected:
    BuildNode(NodeId id_, fs::path src_, fs::path dst_)
        : id(std::move(id_)), src(std::move(src_)), dst(std::move(dst_)) {}
};

// Renders a Markdown source file to HTML, optionally wrapped in a layout.
struct MarkdownNode : BuildNode {
    std::unique_ptr<Processor> renderer;

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

// Applies {{content}} and other token substitutions to a layout template.
struct TemplateNode : BuildNode {
    std::unique_ptr<Processor> engine;

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

// Copies a static asset verbatim to the output directory.
struct AssetNode : BuildNode {
    std::unique_ptr<Processor> copier;

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
