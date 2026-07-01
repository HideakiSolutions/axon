#pragma once
#include "../parser/parser.hpp"
#include <optional>
#include <string>

namespace axon {

enum class CapsuleCompression { Off, Body };

enum class OutputKind {
    SourceCode,
    Json,
    Diff,
    Log,
    Markdown,
    PlainText,
    Binary
};

// Map "off" → Off, "body" → Body; anything else → Off (safe default).
CapsuleCompression compression_from_string(const std::string& s);

// Classify a large output before lossy compression. A language hint is treated
// as authoritative source-code evidence; otherwise the classifier uses cheap,
// deterministic text-shape checks and returns Binary for unsafe byte streams.
OutputKind classify_output(const std::string& source,
                           std::optional<Language> lang = std::nullopt);

std::string output_kind_to_string(OutputKind kind);

// Dense lossless-by-selection projection of `source` fitting ~token_budget tokens.
// Preserves: declaration header lines + structurally significant lines (return /
// throw / control-flow / block openers+closers). Elided runs are replaced by a
// single `// … (N lines elided)` marker. Deterministic, O(n), never throws.
// `lang` is accepted for future language-specific heuristics; v1 is language-light.
// On any error, impossible budget, or binary-like input, returns the original
// source so callers never lose bytes silently.
std::string compress_body(const std::string& source,
                          std::optional<Language> lang,
                          int token_budget);

} // namespace axon
