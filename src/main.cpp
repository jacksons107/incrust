#include "build_graph.hpp"
#include "build_node.hpp"
#include "file_watcher.hpp"
#include "hash_store.hpp"
#include "processor.hpp"
#include "thread_pool.hpp"

#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ─── CLI help ─────────────────────────────────────────────────────────────────

static void print_usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [OPTIONS] <source-dir> <output-dir>\n\n"
        << "  --watch          Rebuild automatically when source files change\n"
        << "  --jobs N         Worker thread count (default: hardware concurrency)\n"
        << "  --cache FILE     Path to cache file (default: .incrust_cache)\n"
        << "  --help           Show this message\n";
}

// ─── Site scanning ────────────────────────────────────────────────────────────

// Walk source_dir and populate the graph.
//
// Convention:
//   content/**/*.md   → MarkdownNode  (rendered through templates/base.html if present)
//   templates/**/*    → TemplateNode  (treated as dependencies, not built directly)
//   assets/**/*       → AssetNode     (copied verbatim)
static void scan_site(BuildGraph<BuildNode>& graph,
                      FileWatcher&           watcher,
                      const fs::path&        source_dir,
                      const fs::path&        output_dir)
{
    const fs::path content_dir  = source_dir / "content";
    const fs::path templates_dir = source_dir / "templates";
    const fs::path assets_dir   = source_dir / "assets";
    const fs::path layout       = templates_dir / "base.html";

    // ── Template nodes (layout files) ────────────────────────────────────────
    if (fs::exists(templates_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(templates_dir)) {
            if (!entry.is_regular_file()) continue;
            const fs::path& src = entry.path();
            const NodeId    id  = src.lexically_relative(source_dir).string();
            const fs::path  dst = output_dir / src.lexically_relative(source_dir);

            auto node = std::make_unique<TemplateNode>(
                id, src, dst,
                std::make_unique<TemplateProcessor>()
            );
            watcher.watch(src, id);
            graph.add_node(std::unique_ptr<BuildNode>(std::move(node)));
        }
    }

    // ── Content nodes (Markdown) ──────────────────────────────────────────────
    if (fs::exists(content_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(content_dir)) {
            if (!entry.is_regular_file()) continue;
            const fs::path& src = entry.path();
            if (src.extension() != ".md") continue;

            const NodeId   id  = src.lexically_relative(source_dir).string();
            fs::path       dst = output_dir
                               / src.lexically_relative(source_dir)
                                    .replace_extension(".html");

            auto proc = std::make_unique<MarkdownProcessor>(
                fs::exists(layout) ? layout : fs::path{}
            );

            auto node = std::make_unique<MarkdownNode>(id, src, dst, std::move(proc));

            // Depend on the layout template so a layout change rebuilds all pages.
            if (fs::exists(layout)) {
                const NodeId layout_id = layout.lexically_relative(source_dir).string();
                if (graph.has_node(layout_id))
                    node->deps.push_back(layout_id);
            }

            watcher.watch(src, id);
            graph.add_node(std::unique_ptr<BuildNode>(std::move(node)));
        }
    }

    // ── Asset nodes (static files) ────────────────────────────────────────────
    if (fs::exists(assets_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(assets_dir)) {
            if (!entry.is_regular_file()) continue;
            const fs::path& src = entry.path();
            const NodeId    id  = src.lexically_relative(source_dir).string();
            const fs::path  dst = output_dir / src.lexically_relative(source_dir);

            auto node = std::make_unique<AssetNode>(
                id, src, dst,
                std::make_unique<CopyProcessor>()
            );
            watcher.watch(src, id);
            graph.add_node(std::unique_ptr<BuildNode>(std::move(node)));
        }
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // ── Argument parsing ──────────────────────────────────────────────────────
    bool        watch_mode  = false;
    std::size_t jobs        = std::thread::hardware_concurrency();
    fs::path    cache_path  = ".incrust_cache";
    fs::path    source_dir;
    fs::path    output_dir;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--watch") {
            watch_mode = true;
        } else if (arg == "--jobs" && i + 1 < argc) {
            jobs = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--cache" && i + 1 < argc) {
            cache_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (source_dir.empty()) {
            source_dir = arg;
        } else if (output_dir.empty()) {
            output_dir = arg;
        } else {
            std::cerr << "Unexpected argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (source_dir.empty() || output_dir.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    if (!fs::exists(source_dir)) {
        std::cerr << "Source directory does not exist: " << source_dir << "\n";
        return 1;
    }

    fs::create_directories(output_dir);

    // ── Initialise subsystems ─────────────────────────────────────────────────
    HashStore   store;
    store.load(cache_path);

    ThreadPool pool(jobs);
    std::cout << "[incrust] thread pool size: " << pool.size() << "\n";

    BuildGraph<BuildNode> graph;
    FileWatcher           watcher;

    scan_site(graph, watcher, source_dir, output_dir);
    graph.mark_all_dirty();  // first run: assume everything is dirty

    // Mutex protecting HashStore from concurrent set() calls in worker threads.
    // (HashStore itself is not thread-safe.)
    std::mutex store_mtx;

    // ── Initial build ─────────────────────────────────────────────────────────
    std::cout << "[incrust] building site: " << source_dir << " → " << output_dir << "\n";
    graph.rebuild_all(pool, store);
    store.save(cache_path);
    std::cout << "[incrust] build complete.\n";

    if (!watch_mode) return 0;

    // ── Watch loop ────────────────────────────────────────────────────────────
    std::cout << "[incrust] watching for changes (Ctrl-C to quit)...\n";
    watcher.start(std::chrono::milliseconds{300});

    while (true) {
        watcher.wait_for_dirty(std::chrono::milliseconds{1000});

        NodeId dirty_id;
        bool   any = false;
        while (watcher.pop_dirty(dirty_id)) {
            if (graph.has_node(dirty_id)) {
                graph.mark_dirty(dirty_id);
                any = true;
            }
        }

        if (any) {
            std::cout << "[incrust] changes detected — rebuilding...\n";
            graph.rebuild_all(pool, store);
            store.save(cache_path);
            std::cout << "[incrust] rebuild complete.\n";
        }
    }
}
