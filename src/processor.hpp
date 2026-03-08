#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

// Abstract base for all content processors.
// Owned exclusively via unique_ptr<Processor> — RAII + polymorphism story.
struct Processor {
    virtual ~Processor() = default;

    // Read src, transform, write to dst.
    virtual void process(const fs::path& src, const fs::path& dst) = 0;

    // Human-readable name for logging.
    virtual std::string_view name() const = 0;
};

// Forward declarations of concrete processors (implemented in processor.cpp)
struct MarkdownProcessor : Processor {
    // Optional layout template path injected at construction time.
    // If set, the rendered HTML body is inserted into the layout's {{content}} slot.
    explicit MarkdownProcessor(fs::path layout = {}) : layout_(std::move(layout)) {}

    void process(const fs::path& src, const fs::path& dst) override;
    std::string_view name() const override { return "MarkdownProcessor"; }

private:
    fs::path layout_;
};

struct CopyProcessor : Processor {
    void process(const fs::path& src, const fs::path& dst) override;
    std::string_view name() const override { return "CopyProcessor"; }
};

// Template processor: reads an .html template, substitutes {{key}} tokens
// using a variable map supplied at build time.
struct TemplateProcessor : Processor {
    using VarMap = std::unordered_map<std::string, std::string>;

    explicit TemplateProcessor(VarMap vars = {}) : vars_(std::move(vars)) {}

    void process(const fs::path& src, const fs::path& dst) override;
    std::string_view name() const override { return "TemplateProcessor"; }

    void set_var(std::string key, std::string val) { vars_[std::move(key)] = std::move(val); }

private:
    VarMap vars_;
};
