# Incrust

An incremental static site generator written in modern C++20.

Incrust works like a build system: it models your site as a dependency graph,
hashes every input file, and only rebuilds the pages whose sources have actually
changed. Independent nodes are rebuilt in parallel across a thread pool. An
optional watch mode detects file changes at runtime and triggers incremental
rebuilds automatically.

---

## Quick Start

### Prerequisites

- CMake 3.20+
- A C++20-capable compiler. On macOS, Homebrew LLVM is recommended:

```bash
brew install llvm cmake
```

### Build

```bash
git clone <repo-url> incrust
cd incrust
mkdir build && cd build
cmake .. -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++
cmake --build .
```

### Run

```bash
# One-shot build
./build/incrust demo_site output

# Watch mode — rebuilds when any source file changes
./build/incrust demo_site output --watch

# Build and preview in a browser (default port 8080)
./build/incrust demo_site output --serve

# Watch AND serve simultaneously — edit a file and refresh the browser
./build/incrust demo_site output --watch --serve

# Custom port
./build/incrust demo_site output --serve 3000

# Options
./build/incrust --help
```

```
Usage: incrust [OPTIONS] <source-dir> <output-dir>

  --watch          Rebuild automatically when source files change
  --serve [PORT]   Serve the output directory (default port: 8080)
  --jobs N         Worker thread count (default: hardware concurrency)
  --cache FILE     Path to cache file (default: .incrust_cache)
  --help           Show this message
```

When `--serve` is active, open [http://localhost:8080/content/index.html](http://localhost:8080/content/index.html)
in your browser. The CSS uses absolute paths, so the site must be served — opening
the HTML files directly from the filesystem will not load styles correctly.

---

## Site Structure

```
my-site/
├── content/          # Markdown source files (.md → .html)
│   ├── index.md
│   └── about.md
├── templates/        # HTML layout files
│   └── base.html     # Injected via {{content}} / {{title}} tokens
└── assets/           # Static files copied verbatim
    └── css/
        └── style.css
```

Output mirrors the source structure under `<output-dir>/`.

### Layout templates

Any `{{token}}` in a template is replaced at build time. Supported tokens:

| Token | Value |
|---|---|
| `{{content}}` | Rendered HTML body of the Markdown page |
| `{{title}}` | Stem of the source filename (e.g. `about` from `about.md`) |

### Incrementality

After each build, Incrust writes a `.incrust_cache` file next to the binary:

```
# incrust cache — do not edit manually
content/index.md=9b12e04412fa8b23
templates/base.html=a3f5c2d17e009011
assets/css/style.css=ff1a2b3c44d5e6f7
```

On the next run the file is loaded and each source is hashed again. A node is
skipped if its hash is unchanged. Delete the cache file to force a full rebuild.

---

## Implementation

### Architecture overview

```
main.cpp
  │
  ├─► scan_site()           Build the graph from the source directory tree
  │     ├── TemplateNode    templates/**/*
  │     ├── MarkdownNode    content/**/*.md  (depends on base.html)
  │     └── AssetNode       assets/**/*
  │
  ├─► HashStore             Load / save .incrust_cache
  ├─► ThreadPool            Fixed pool of std::jthread workers
  ├─► BuildGraph<BuildNode> Topo-sort → parallel rebuild → cache update
  │     │
  │     └─► FileWatcher (--watch only)
  │           std::jthread poll loop → dirty_queue → mark_dirty → rebuild
  │
  └─► HttpServer (--serve only)
        std::jthread + httplib → serves output dir on localhost
```

### Source files

| File | Responsibility |
|---|---|
| `build_node.hpp` | `BuildNode` abstract base + `MarkdownNode`, `TemplateNode`, `AssetNode` |
| `build_graph.hpp` | `BuildGraph<NodeT>` template — owns nodes, computes topo order |
| `build_graph_impl.hpp` | `rebuild_all()` — hash-gated, dependency-aware parallel dispatch |
| `processor.hpp/.cpp` | `Processor` polymorphic hierarchy — Markdown renderer, template engine, copy |
| `hash_store.hpp` | FNV-1a file hashing, flat key=value cache persistence |
| `thread_pool.hpp` | Fixed `std::jthread` worker pool with `std::condition_variable` barrier |
| `file_watcher.hpp` | `std::jthread` poll loop, `std::mutex`-guarded dirty queue |
| `http_server.hpp` | RAII HTTP server — `std::jthread` + `std::stop_callback` shutdown |
| `main.cpp` | CLI, site scanner, initial build, optional watch/serve loop |

---

## Modern C++ Features

### `unique_ptr` and ownership

Every `BuildNode` is owned by the graph through `std::unique_ptr<NodeT>`:

```cpp
// build_graph.hpp
std::unordered_map<NodeId, std::unique_ptr<NodeT>> nodes_;
```

Each concrete node type holds its `Processor` via `unique_ptr<Processor>`,
making the ownership chain explicit and leak-free with no raw `new` or `delete`
anywhere in the codebase:

```cpp
// build_node.hpp
struct MarkdownNode : BuildNode {
    std::unique_ptr<Processor> renderer;
    ...
};
```

### Templates and C++20 concepts

`BuildGraph` is a class template constrained by a custom C++20 concept that
enforces the `BuildNode` base at compile time:

```cpp
// build_graph.hpp
template<typename T>
concept BuildNodeType = std::derived_from<T, BuildNode>;

template<BuildNodeType NodeT>
class BuildGraph { ... };
```

This gives a clear compiler error if someone instantiates the graph with an
unrelated type, rather than an obscure deep-template failure.

### `std::jthread` — automatic join and stop tokens

Both the `ThreadPool` and the `FileWatcher` use `std::jthread`. The key
advantage over `std::thread` is cooperative cancellation via `std::stop_token`
and RAII auto-join on destruction — no manual `join()` or `detach()` calls:

```cpp
// thread_pool.hpp — workers check their stop token on every iteration
void worker_loop(std::stop_token stoken) {
    while (!stoken.stop_requested()) {
        std::unique_lock lk(mtx_);
        cv_work_.wait(lk, [&] {
            return !tasks_.empty() || stoken.stop_requested();
        });
        ...
    }
}

// Destructor: request stop, then jthread dtors auto-join
~ThreadPool() {
    for (auto& w : workers_) w.request_stop();
    cv_work_.notify_all();
}
```

```cpp
// file_watcher.hpp — single jthread polls mtimes on a configurable interval
watcher_thread_ = std::jthread([this](std::stop_token st) {
    poll_loop(st);
});
```

The `HttpServer` takes this one step further with `std::stop_callback` (also
C++20), which registers a callable that fires automatically when the stop token
is triggered — no polling, no flag variable:

```cpp
// http_server.hpp
server_thread_ = std::jthread([this](std::stop_token st) {
    std::stop_callback on_stop(st, [this] { svr_.stop(); });
    svr_.listen("localhost", port_);   // blocks until svr_.stop() is called
});
```

When the `HttpServer` is destroyed, the jthread destructor requests a stop,
`on_stop` fires synchronously and calls `svr_.stop()`, the listen loop exits,
and the thread joins — all in the correct order with no manual coordination.

### `std::mutex` and `std::condition_variable` — producer/consumer

The `ThreadPool` uses two condition variables: one to signal new work to idle
workers, and one to signal task completion back to `wait_all()`:

```cpp
// thread_pool.hpp
std::condition_variable cv_work_;   // producer → workers
std::condition_variable cv_done_;   // workers → wait_all()
```

The `FileWatcher` uses the same pattern between its background poll thread
(producer) and the main thread (consumer via `pop_dirty` / `wait_for_dirty`):

```cpp
// file_watcher.hpp
dirty_queue_.push(entry.node_id);
cv_.notify_all();                   // wake main thread

// main thread:
watcher.wait_for_dirty(1000ms);     // sleeps on cv_ until event arrives
```

### `std::atomic` — lock-free flags

`rebuild_all` tracks per-node completion with a map of `std::atomic<bool>`
flags so that the main dispatch loop can check dependency readiness without
holding the condition variable's lock:

```cpp
// build_graph_impl.hpp
std::unordered_map<NodeId, std::atomic<bool>> done_flags;

auto deps_done = [&](const NodeId& id) -> bool {
    return std::ranges::all_of(node->deps, [&](const NodeId& dep) {
        return done_flags.at(dep).load();
    });
};
```

### `std::function<void()>` — type-erased build actions

Each node stores its build action as a `std::function<void()>`. This lets the
graph dispatch any node to the thread pool without knowing its concrete type:

```cpp
// build_node.hpp
std::function<void()> build_action;

// MarkdownNode sets it at construction:
build_action = [this] {
    fs::create_directories(this->dst.parent_path());
    renderer->process(this->src, this->dst);
};

// build_graph_impl.hpp dispatches it without caring what it does:
pool.submit([&, id]() { n->build_action(); });
```

### `std::filesystem`

All path operations — directory traversal, mtime polling, output path
construction, and `create_directories` — use `std::filesystem` throughout. No
platform-specific `opendir`/`stat` calls appear anywhere:

```cpp
for (const auto& entry : fs::recursive_directory_iterator(content_dir)) {
    fs::path dst = output_dir / entry.path()
                                    .lexically_relative(source_dir)
                                    .replace_extension(".html");
    ...
}
```

### `std::ranges::all_of`

Used in `rebuild_all` to check dependency readiness in a single readable
expression:

```cpp
return std::ranges::all_of(node->deps, [&](const NodeId& dep) {
    return done_flags.at(dep).load();
});
```

### RAII throughout

Every resource is tied to an object's lifetime. There are no bare `new`,
`delete`, `pthread_create`, or `fopen` calls in the codebase. File handles are
`std::ifstream`/`std::ofstream` (close on scope exit), threads are `jthread`
(join on scope exit), and node memory is `unique_ptr` (free on graph
destruction).

---

## Markdown support

The built-in renderer handles:

- ATX headings (`#` through `######`)
- **Bold** (`**text**`), *italic* (`*text*`), `inline code` (`` `text` ``)
- Fenced code blocks (` ``` `)
- Unordered lists (`- item`)
- Horizontal rules (`---`)
- Blank-line delimited paragraphs
- HTML entity escaping (`&`, `<`, `>`, `"`)

---

## Local preview server

The `--serve` flag starts an HTTP server that mounts the output directory at
`/` and serves files to the browser. It is implemented in `src/http_server.hpp`
using [cpp-httplib](https://github.com/yhirose/cpp-httplib), a single-header
library that is downloaded automatically at CMake configure time if not already
present in `src/vendor/`.

The server runs on its own `std::jthread` so it never blocks the main thread.
This means `--serve` and `--watch` can run simultaneously: the watcher thread
detects changes, the thread pool rebuilds affected nodes, and the browser can
be refreshed to see the result — all concurrently.

Shutdown is handled via `std::stop_callback`: when the process exits and the
`HttpServer` destructor runs, the jthread's stop token fires, the callback calls
`svr_.stop()`, and the listener unblocks and joins cleanly.

---

## Hashing

Source files are hashed with 64-bit FNV-1a, streamed through a 4 KB buffer.
No external library is required. FNV-1a is not cryptographically secure but
has near-zero false-negative probability for the change-detection use case and
produces a deterministic result for a given file across runs.
