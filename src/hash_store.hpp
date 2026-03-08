#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

// Persistent key→hash store backed by a flat text file (.incrust_cache).
//
// File format — one entry per line:
//   <node-id>=<hex-hash>
//
// Example:
//   content/index.md=a3f5c2d1
//   templates/base.html=9b12e044
//
// Thread safety: NOT thread-safe on its own.  The thread pool calls
// HashStore::set() from worker threads, so callers must synchronise
// externally (a per-store std::mutex in main.cpp is sufficient).

class HashStore {
public:
    // ── Persistence ──────────────────────────────────────────────────────────

    void load(const fs::path& cache_file) {
        cache_path_ = cache_file;
        store_.clear();

        std::ifstream f(cache_file);
        if (!f.is_open()) return;   // no cache yet — first run is fine

        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line.front() == '#') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            store_.emplace(line.substr(0, eq), line.substr(eq + 1));
        }
    }

    void save(const fs::path& cache_file) const {
        std::ofstream f(cache_file, std::ios::trunc);
        if (!f.is_open())
            throw std::runtime_error("HashStore: cannot write cache to " + cache_file.string());

        f << "# incrust cache — do not edit manually\n";
        for (const auto& [key, hash] : store_)
            f << key << '=' << hash << '\n';
    }

    // Convenience overload — save to the path used at load time.
    void save() const { save(cache_path_); }

    // ── Accessors ─────────────────────────────────────────────────────────────

    // Returns the stored hash for key, or "" if not found.
    [[nodiscard]] std::string get(const std::string& key) const {
        auto it = store_.find(key);
        return it != store_.end() ? it->second : std::string{};
    }

    void set(const std::string& key, const std::string& hash) {
        store_[key] = hash;
    }

    bool has(const std::string& key) const { return store_.contains(key); }

    // ── Static file hashing ───────────────────────────────────────────────────

    // Returns a hex string representing a fast non-cryptographic hash of the
    // file contents.  We use FNV-1a (64-bit) — no external dependencies and
    // more than sufficient for change detection.
    [[nodiscard]] static std::string hash_file(const fs::path& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return "";

        constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
        constexpr uint64_t FNV_PRIME  = 1099511628211ULL;

        uint64_t hash = FNV_OFFSET;
        char buf[4096];
        while (f.read(buf, sizeof buf), f.gcount() > 0) {
            for (std::streamsize i = 0; i < f.gcount(); ++i) {
                hash ^= static_cast<uint8_t>(buf[i]);
                hash *= FNV_PRIME;
            }
        }

        // Format as 16-char hex string.
        std::ostringstream oss;
        oss << std::hex << hash;
        return oss.str();
    }

private:
    fs::path cache_path_;
    std::unordered_map<std::string, std::string> store_;
};
