#include "compress.hpp"
#include <string>
#include <vector>
#include <cstring>
#include <cctype>

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

// Replace invalid UTF-8 sequences with '?' — mirrors capsule.cpp's sanitize_utf8.
static std::string sanitize_utf8_local(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t bytes = 0;
        if      (c < 0x80)           bytes = 1;
        else if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        else { out += '?'; i++; continue; }
        if (i + bytes > s.size()) { out += '?'; break; }
        bool ok = true;
        for (size_t j = 1; j < bytes; j++) {
            if ((((unsigned char)s[i + j]) & 0xC0) != 0x80) { ok = false; break; }
        }
        if (!ok) { out += '?'; i++; continue; }
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
    static const char* const kws[] = {
        "return", "throw", "raise", "if", "for", "while",
        "switch", "case", "match", "yield", "await",
        "break", "continue", "goto", nullptr
    };
    for (const char* const* kw = kws; *kw; ++kw) {
        size_t kwlen = strlen(*kw);
        size_t pos   = 0;
        while ((pos = line.find(*kw, pos)) != std::string::npos) {
            bool left_ok  = (pos == 0 || !std::isalnum((unsigned char)line[pos - 1]));
            size_t end    = pos + kwlen;
            bool right_ok = (end >= line.size() || !std::isalnum((unsigned char)line[end]));
            if (left_ok && right_ok) return true;
            pos = end;
        }
    }
    return false;
}

std::string compress_body(const std::string& source,
                          std::optional<Language> /*lang*/,
                          int token_budget) try {
    // Step 1: already within budget — return unchanged (byte-identical fast path).
    if (est_tokens(source) <= token_budget) return source;

    // Split into lines, keeping the '\n' terminator attached to each line so
    // re-joining is a simple concatenation.
    std::vector<std::string> lines;
    lines.reserve(64);
    {
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
    }
    if (lines.empty()) return source;

    // Step 2: classify each line as kept or elided.
    // Always keep: header block = lines up to (and including) the first line that
    // ends with '{' or ':' (opening brace / Python-style colon), capped at
    // HEADER_CEIL lines so a file that opens with a long docblock doesn't balloon.
    const int HEADER_CEIL = 8;
    bool header_done = false;
    std::vector<bool> keep(lines.size(), false);

    for (size_t i = 0; i < lines.size(); ++i) {
        if (!header_done) {
            keep[i] = true;
            // Scan backwards past whitespace to find the last non-whitespace char.
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

    // Step 3: assemble output; collapse consecutive elided lines into one marker.
    std::string result;
    result.reserve(source.size() / 2);

    size_t i = 0;
    while (i < lines.size()) {
        if (keep[i]) {
            result += lines[i];
            ++i;
        } else {
            size_t run_start = i;
            while (i < lines.size() && !keep[i]) ++i;
            size_t elided = i - run_start;
            result += "// … (" + std::to_string(elided) + " lines elided)\n";
        }
        // Stop adding once we've reached the budget to avoid unnecessary work.
        if (est_tokens(result) >= token_budget) break;
    }

    // Step 4: hard byte-clamp if still over budget (e.g. a single huge kept line).
    if (est_tokens(result) > token_budget) {
        size_t max_bytes = (size_t)token_budget * 4;
        if (result.size() > max_bytes) result.resize(max_bytes);
    }

    return sanitize_utf8_local(result);

} catch (...) {
    // Invariant #3: never throws. Fall back to a sanitized head-slice.
    try {
        std::string safe = sanitize_utf8_local(source);
        size_t max_bytes = (size_t)std::max(token_budget, 1) * 4;
        if (safe.size() > max_bytes) safe.resize(max_bytes);
        return safe;
    } catch (...) {
        return {};
    }
}

} // namespace axon
