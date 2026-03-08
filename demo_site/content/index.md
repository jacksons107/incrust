# Welcome to Incrust

Incrust is an **incremental static site generator** written in modern C++20.

## Features

- Incremental builds — only changed files are rebuilt
- Parallel processing via a `std::jthread` thread pool
- Dependency-aware DAG with topological ordering
- File watching with a producer/consumer queue
- Zero runtime dependencies

## How it works

Incrust scans your `content/`, `templates/`, and `assets/` directories
and constructs a **build graph**.  Each node stores its source path, output
path, dependencies, and a build action wrapped in a `std::function<void()>`.

Before rebuilding any node, Incrust hashes the source file using FNV-1a.
If the hash matches the value stored in `.incrust_cache`, the node is skipped —
saving time on large sites.

Independent nodes are dispatched concurrently to a fixed thread pool backed
by `std::jthread` workers.

---

Get started by editing the files in `demo_site/content/` and re-running:

```
./incrust demo_site output
```
