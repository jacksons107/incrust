# About Incrust

Incrust is a course project demonstrating **modern C++20** features including:

- `unique_ptr` ownership throughout — no raw `new` or `delete`
- `BuildGraph<NodeT>` — a templated DAG constrained by a C++20 concept
- `std::jthread` workers — auto-joining threads with stop-token support
- `std::mutex` and `std::condition_variable` — producer/consumer coordination
- `std::atomic<bool>` — lock-free shutdown signalling
- `std::function<void()>` — type-erased build actions
- `std::filesystem` — portable path and directory operations
- `std::ranges::all_of` — range algorithms over dependency lists

## Architecture

```
BuildGraph<BuildNode>
  ├── MarkdownNode  ──► MarkdownProcessor
  ├── TemplateNode  ──► TemplateProcessor
  └── AssetNode     ──► CopyProcessor

FileWatcher (jthread)
  └── dirty_queue (mutex + condition_variable)
        └── main thread → mark_dirty() → rebuild_all()

ThreadPool (vector<jthread>)
  └── task queue (mutex + condition_variable)
```

## Design decisions

**Why FNV-1a for hashing?**
No external dependencies, constant memory, and more than sufficient
collision resistance for change detection on a local filesystem.

**Why a flat cache file?**
Plain text key=value pairs are easy to inspect, diff, and delete.
A corrupt cache is never fatal — Incrust simply rebuilds everything.

