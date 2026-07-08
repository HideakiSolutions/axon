#include "compress.hpp"
#include <string>
#include <vector>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace axon {

// Local token estimator — mirrors capsule.hpp's inline estimate_tokens to keep
// compress.cpp self-contained (avoids pulling in DuckDB/graph headers).
static inline int est_tokens(const std::string& s) {
    return (int)((s.size() + 3) / 4);
}

CapsuleCompression compression_from_string(const std::string& s) {
    if (s == "body") return CapsuleCompression::Body;
    return CapsuleCompression::Off;
}

std::string output_kind_to_string(OutputKind kind) {
    switch (kind) {
    case OutputKind::SourceCode:
        return "source_code";
    case OutputKind::Json:
        return "json";
    case OutputKind::Diff:
        return "diff";
    case OutputKind::Log:
        return "log";
    case OutputKind::Markdown:
        return "markdown";
    case OutputKind::PlainText:
        return "plain_text";
    case OutputKind::Binary:
        return "binary";
    }
    return "plain_text";
}

static std::string trim_copy(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace((unsigned char)s[start]))
        ++start;
    size_t end = s.size();
    while (end > start && std::isspace((unsigned char)s[end - 1]))
        --end;
    return s.substr(start, end - start);
}

static std::vector<std::string> split_lines_keep_newline(const std::string& source) {
    std::vector<std::string> lines;
    lines.reserve(64);
    size_t pos = 0;
    while (pos < source.size()) {
        size_t nl = source.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(source.substr(pos));
            break;
        }
        lines.push_back(source.substr(pos, nl - pos + 1));
        pos = nl + 1;
    }
    return lines;
}

static bool has_word_ci(const std::string& line, const char* word) {
    std::string lower;
    lower.reserve(line.size());
    for (char c : line)
        lower += (char)std::tolower((unsigned char)c);
    return lower.find(word) != std::string::npos;
}

static bool looks_like_timestamped_log(const std::string& line) {
    if (line.size() >= 19 && std::isdigit((unsigned char)line[0]) &&
        std::isdigit((unsigned char)line[1]) && std::isdigit((unsigned char)line[2]) &&
        std::isdigit((unsigned char)line[3]) && line[4] == '-' && line[7] == '-' &&
        (line[10] == 'T' || line[10] == ' ')) {
        return true;
    }
    return line.size() >= 8 && std::isdigit((unsigned char)line[0]) &&
           std::isdigit((unsigned char)line[1]) && line[2] == ':' &&
           std::isdigit((unsigned char)line[3]) && std::isdigit((unsigned char)line[4]) &&
           line[5] == ':';
}

OutputKind classify_output(const std::string& source, std::optional<Language> lang) {
    if (source.empty()) return OutputKind::PlainText;
    if (lang.has_value()) return OutputKind::SourceCode;

    int control = 0;
    int sampled = 0;
    const int limit = std::min<int>((int)source.size(), 4096);
    for (int i = 0; i < limit; ++i) {
        unsigned char c = (unsigned char)source[(size_t)i];
        if (c == '\0') return OutputKind::Binary;
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\x1b') ++control;
        ++sampled;
    }
    if (sampled > 0 && control * 100 / sampled > 5) return OutputKind::Binary;

    std::string trimmed = trim_copy(source);
    if (!trimmed.empty()) {
        char first = trimmed.front();
        char last = trimmed.back();
        if ((first == '{' && last == '}') || (first == '[' && last == ']')) return OutputKind::Json;
    }

    auto lines = split_lines_keep_newline(source);
    int diff_markers = 0;
    int log_markers = 0;
    int markdown_markers = 0;
    int code_markers = 0;
    int inspected = 0;
    for (const auto& line : lines) {
        if (inspected++ >= 80) break;
        std::string t = trim_copy(line);
        if (t.rfind("diff --git ", 0) == 0 || t.rfind("@@ ", 0) == 0 || t.rfind("+++", 0) == 0 ||
            t.rfind("---", 0) == 0) {
            ++diff_markers;
        }
        if (looks_like_timestamped_log(t) || has_word_ci(t, " error") ||
            has_word_ci(t, "[error]") || has_word_ci(t, " warn") || has_word_ci(t, "[warn]") ||
            has_word_ci(t, " info") || has_word_ci(t, "[info]")) {
            ++log_markers;
        }
        if (t.rfind("# ", 0) == 0 || t.rfind("## ", 0) == 0 || t.rfind("```", 0) == 0 ||
            t.rfind("- ", 0) == 0) {
            ++markdown_markers;
        }
        if (t.rfind("def ", 0) == 0 || t.rfind("class ", 0) == 0 || t.rfind("function ", 0) == 0 ||
            t.rfind("import ", 0) == 0 || t.rfind("#include ", 0) == 0 ||
            t.find(") {") != std::string::npos || t.find(" => ") != std::string::npos ||
            t.find("return ") != std::string::npos) {
            ++code_markers;
        }
    }

    if (diff_markers >= 2) return OutputKind::Diff;
    if (log_markers >= 2) return OutputKind::Log;
    if (code_markers >= 1) return OutputKind::SourceCode;
    if (markdown_markers >= 2) return OutputKind::Markdown;
    return OutputKind::PlainText;
}

// Replace invalid UTF-8 sequences with '?' — mirrors capsule.cpp's sanitize_utf8.
static std::string sanitize_utf8_local(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t bytes = 0;
        if (c < 0x80)
            bytes = 1;
        else if ((c & 0xE0) == 0xC0)
            bytes = 2;
        else if ((c & 0xF0) == 0xE0)
            bytes = 3;
        else if ((c & 0xF8) == 0xF0)
            bytes = 4;
        else {
            out += '?';
            i++;
            continue;
        }
        if (i + bytes > s.size()) {
            out += '?';
            break;
        }
        bool ok = true;
        for (size_t j = 1; j < bytes; j++) {
            if ((((unsigned char)s[i + j]) & 0xC0) != 0x80) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            out += '?';
            i++;
            continue;
        }
        out.append(s, i, bytes);
        i += bytes;
    }
    return out;
}

// Returns true if `line` contains a control-flow keyword or a block boundary.
// Language-light heuristic: works across C-family, Python, Rust, Go, Java, etc.
static bool is_significant_line(const std::string& line) {
    // Block openers/closers at end of (trimmed) line
    for (auto it = line.rbegin(); it != line.rend(); ++it) {
        char ch = *it;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        if (ch == '{' || ch == '}') return true;
        break;
    }
    // Whole-word control-flow keywords
    static const char* const kws[] = {"return", "throw",  "raise",    "if",    "for",
                                      "while",  "switch", "case",     "match", "yield",
                                      "await",  "break",  "continue", "goto",  nullptr};
    for (const char* const* kw = kws; *kw; ++kw) {
        size_t kwlen = strlen(*kw);
        size_t pos = 0;
        while ((pos = line.find(*kw, pos)) != std::string::npos) {
            bool left_ok = (pos == 0 || !std::isalnum((unsigned char)line[pos - 1]));
            size_t end = pos + kwlen;
            bool right_ok = (end >= line.size() || !std::isalnum((unsigned char)line[end]));
            if (left_ok && right_ok) return true;
            pos = end;
        }
    }
    return false;
}

static std::string compress_source_lines(const std::vector<std::string>& lines) {
    if (lines.empty()) return {};

    const int HEADER_CEIL = 8;
    bool header_done = false;
    std::vector<bool> keep(lines.size(), false);

    for (size_t i = 0; i < lines.size(); ++i) {
        if (!header_done) {
            keep[i] = true;
            const std::string& l = lines[i];
            for (auto it = l.rbegin(); it != l.rend(); ++it) {
                char ch = *it;
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
                if (ch == '{' || ch == ':') header_done = true;
                break;
            }
            if (!header_done && (int)i + 1 >= HEADER_CEIL) header_done = true;
            continue;
        }
        if (is_significant_line(lines[i])) keep[i] = true;
    }

    std::string result;
    result.reserve(1024);
    size_t i = 0;
    while (i < lines.size()) {
        if (keep[i]) {
            result += lines[i];
            ++i;
        } else {
            size_t run_start = i;
            while (i < lines.size() && !keep[i])
                ++i;
            size_t elided = i - run_start;
            result += "// … (" + std::to_string(elided) + " lines elided)\n";
        }
    }
    return result;
}

static std::string compress_log_lines(const std::vector<std::string>& lines) {
    std::string result;
    size_t omitted = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        bool edge = i < 4 || i + 4 >= lines.size();
        bool important = has_word_ci(lines[i], "error") || has_word_ci(lines[i], "warn") ||
                         has_word_ci(lines[i], "fatal") || has_word_ci(lines[i], "exception") ||
                         has_word_ci(lines[i], "failed");
        if (edge || important) {
            if (omitted > 0) {
                result += "// … (" + std::to_string(omitted) + " log lines elided)\n";
                omitted = 0;
            }
            result += lines[i];
        } else {
            ++omitted;
        }
    }
    if (omitted > 0) result += "// … (" + std::to_string(omitted) + " log lines elided)\n";
    return result;
}

static std::string compress_diff_or_markdown_lines(const std::vector<std::string>& lines,
                                                   bool diff) {
    std::string result;
    size_t omitted = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        std::string t = trim_copy(line);
        bool keep = i < 8 || i + 8 >= lines.size();
        if (diff) {
            keep = keep || t.rfind("diff --git ", 0) == 0 || t.rfind("@@ ", 0) == 0 ||
                   t.rfind("+++", 0) == 0 || t.rfind("---", 0) == 0 || t.rfind("+", 0) == 0 ||
                   t.rfind("-", 0) == 0;
        } else {
            keep = keep || t.rfind("#", 0) == 0 || t.rfind("```", 0) == 0;
        }
        if (keep) {
            if (omitted > 0) {
                result += "// … (" + std::to_string(omitted) + " lines elided)\n";
                omitted = 0;
            }
            result += line;
        } else {
            ++omitted;
        }
    }
    if (omitted > 0) result += "// … (" + std::to_string(omitted) + " lines elided)\n";
    return result;
}

static std::string compress_plain_text_lines(const std::vector<std::string>& lines) {
    std::string result;
    for (size_t i = 0; i < lines.size() && i < 20; ++i)
        result += lines[i];
    if (lines.size() > 40) {
        result += "// … (" + std::to_string(lines.size() - 40) + " lines elided)\n";
        for (size_t i = lines.size() - 20; i < lines.size(); ++i)
            result += lines[i];
    } else if (lines.size() > 20) {
        result += "// … (" + std::to_string(lines.size() - 20) + " lines elided)\n";
    }
    return result;
}

std::string compress_body(const std::string& source, std::optional<Language> lang,
                          int token_budget) try {
    if (token_budget <= 0) return source;

    // Step 1: already within budget — return unchanged (byte-identical fast path).
    const int original_tokens = est_tokens(source);
    if (original_tokens <= token_budget) return source;

    OutputKind kind = classify_output(source, lang);
    if (kind == OutputKind::Binary) return source;

    auto lines = split_lines_keep_newline(source);
    if (lines.empty()) return source;

    std::string result;
    switch (kind) {
    case OutputKind::SourceCode:
        result = compress_source_lines(lines);
        break;
    case OutputKind::Log:
        result = compress_log_lines(lines);
        break;
    case OutputKind::Diff:
        result = compress_diff_or_markdown_lines(lines, true);
        break;
    case OutputKind::Markdown:
        result = compress_diff_or_markdown_lines(lines, false);
        break;
    case OutputKind::Json:
    case OutputKind::PlainText:
        result = compress_plain_text_lines(lines);
        break;
    case OutputKind::Binary:
        return source;
    }
    if (result.empty()) return source;

    // Step 4: hard byte-clamp if still over budget (e.g. a single huge kept line).
    if (est_tokens(result) > token_budget) {
        size_t max_bytes = (size_t)token_budget * 4;
        if (result.size() > max_bytes) result.resize(max_bytes);
    }

    result = sanitize_utf8_local(result);
    if (est_tokens(result) >= original_tokens) return source;
    return result;

} catch (...) {
    return source;
}

} // namespace axon
