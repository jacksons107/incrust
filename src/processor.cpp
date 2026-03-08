#include "processor.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("processor: cannot read " + path.string());
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

static void write_file(const fs::path& path, std::string_view content) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open())
        throw std::runtime_error("processor: cannot write " + path.string());
    f << content;
}

// Apply {{key}} substitutions from vars to text.
static std::string apply_vars(std::string text,
                               const std::unordered_map<std::string,std::string>& vars)
{
    for (const auto& [key, val] : vars) {
        const std::string token = "{{" + key + "}}";
        std::string::size_type pos = 0;
        while ((pos = text.find(token, pos)) != std::string::npos) {
            text.replace(pos, token.size(), val);
            pos += val.size();
        }
    }
    return text;
}

// ─── Markdown renderer ────────────────────────────────────────────────────────
//
// Single-pass line-oriented renderer.  Supported syntax:
//   # – ###### ATX headings
//   **bold**, *italic*, `inline code`
//   ```...``` fenced code blocks
//   --- horizontal rule
//   Blank-line delimited paragraphs
//   - list items (unordered)
//
// This is intentionally minimal — the goal is to demonstrate modern C++ idioms,
// not to replace a full CommonMark implementation.

static std::string escape_html(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;";  break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '"': out += "&quot;"; break;
            default:  out += c;
        }
    }
    return out;
}

// Apply inline markup: **bold**, *italic*, `code`
static std::string render_inline(const std::string& line) {
    // Order matters: process ** before * to avoid partial matches.
    static const std::regex bold  (R"(\*\*(.+?)\*\*)");
    static const std::regex italic(R"(\*(.+?)\*)");
    static const std::regex code  (R"(`(.+?)`)");

    std::string s = escape_html(line);
    s = std::regex_replace(s, bold,   "<strong>$1</strong>");
    s = std::regex_replace(s, italic, "<em>$1</em>");
    s = std::regex_replace(s, code,   "<code>$1</code>");
    return s;
}

static std::string render_markdown(const std::string& src) {
    std::istringstream in(src);
    std::ostringstream out;

    std::string line;
    bool in_paragraph  = false;
    bool in_code_block = false;
    bool in_list       = false;

    auto close_paragraph = [&] {
        if (in_paragraph) { out << "</p>\n"; in_paragraph = false; }
    };
    auto close_list = [&] {
        if (in_list) { out << "</ul>\n"; in_list = false; }
    };

    while (std::getline(in, line)) {
        // ── Fenced code block ────────────────────────────────────────────────
        if (line.starts_with("```")) {
            if (!in_code_block) {
                close_paragraph();
                close_list();
                out << "<pre><code>";
                in_code_block = true;
            } else {
                out << "</code></pre>\n";
                in_code_block = false;
            }
            continue;
        }
        if (in_code_block) {
            out << escape_html(line) << '\n';
            continue;
        }

        // ── ATX headings ─────────────────────────────────────────────────────
        if (line.starts_with("#")) {
            close_paragraph();
            close_list();
            int level = 0;
            while (level < static_cast<int>(line.size()) && line[level] == '#')
                ++level;
            level = std::clamp(level, 1, 6);
            const std::string text = render_inline(
                line.substr(static_cast<std::string::size_type>(level + 1)));
            out << "<h" << level << '>' << text << "</h" << level << ">\n";
            continue;
        }

        // ── Horizontal rule ───────────────────────────────────────────────────
        if (line == "---" || line == "***") {
            close_paragraph();
            close_list();
            out << "<hr>\n";
            continue;
        }

        // ── Unordered list item ───────────────────────────────────────────────
        if (line.starts_with("- ") || line.starts_with("* ")) {
            close_paragraph();
            if (!in_list) { out << "<ul>\n"; in_list = true; }
            out << "<li>" << render_inline(line.substr(2)) << "</li>\n";
            continue;
        }

        // ── Blank line ────────────────────────────────────────────────────────
        if (line.empty()) {
            close_paragraph();
            close_list();
            continue;
        }

        // ── Paragraph text ────────────────────────────────────────────────────
        close_list();
        if (!in_paragraph) {
            out << "<p>";
            in_paragraph = true;
        } else {
            out << ' ';   // join continuation lines with a space
        }
        out << render_inline(line);
    }

    close_paragraph();
    close_list();
    if (in_code_block) out << "</code></pre>\n";  // unterminated block — close it

    return out.str();
}

// ─── MarkdownProcessor ────────────────────────────────────────────────────────

void MarkdownProcessor::process(const fs::path& src, const fs::path& dst) {
    const std::string md   = read_file(src);
    const std::string body = render_markdown(md);

    std::string output;
    if (!layout_.empty() && fs::exists(layout_)) {
        // Inject body HTML into the layout's {{content}} slot.
        const std::string tmpl = read_file(layout_);
        std::unordered_map<std::string,std::string> vars{
            {"content", body},
            {"title",   src.stem().string()},
        };
        output = apply_vars(tmpl, vars);
    } else {
        // No layout — emit a minimal standalone HTML document.
        output = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"
                 "<title>" + src.stem().string() + "</title></head>\n"
                 "<body>\n" + body + "</body>\n</html>\n";
    }

    write_file(dst, output);
}

// ─── CopyProcessor ────────────────────────────────────────────────────────────

void CopyProcessor::process(const fs::path& src, const fs::path& dst) {
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
}

// ─── TemplateProcessor ───────────────────────────────────────────────────────

void TemplateProcessor::process(const fs::path& src, const fs::path& dst) {
    std::string text = read_file(src);
    text = apply_vars(std::move(text), vars_);
    write_file(dst, text);
}
