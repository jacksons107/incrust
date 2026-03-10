#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

// Abstract base for all content processors.
struct Processor {
    virtual ~Processor() = default;

    virtual void process(const fs::path& src, const fs::path& dst) = 0;
    virtual std::string_view name() const = 0;
};

struct MarkdownProcessor : Processor {
    // If layout is set, the rendered HTML body is injected into its {{content}} slot.
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

// Reads an .html template and substitutes {{key}} tokens using a variable map.
struct TemplateProcessor : Processor {
    using VarMap = std::unordered_map<std::string, std::string>;

    explicit TemplateProcessor(VarMap vars = {}) : vars_(std::move(vars)) {}

    void process(const fs::path& src, const fs::path& dst) override;
    std::string_view name() const override { return "TemplateProcessor"; }

    void set_var(std::string key, std::string val) { vars_[std::move(key)] = std::move(val); }

private:
    VarMap vars_;
};
