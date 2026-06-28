#pragma once
#include "../parser/parser.hpp"
#include <optional>
#include <string>

namespace axon {

enum class CapsuleCompression { Off, Body };

// Map "off" → Off, "body" → Body; anything else → Off (safe default).
CapsuleCompression compression_from_string(const std::string& s);

// Dense lossless-by-selection projection of `source` fitting ~token_budget tokens.
// Preserves: declaration header lines + structurally significant lines (return /
// throw / control-flow / block openers+closers). Elided runs are replaced by a
// single `// … (N lines elided)` marker. Deterministic, O(n), never throws.
// `lang` is accepted for future language-specific heuristics; v1 is language-light.
// On any error or malformed input (invalid UTF-8, binary) falls back to a safe
// head-slice so callers never see an exception.
std::string compress_body(const std::string& source,
                          std::optional<Language> lang,
                          int token_budget);

} // namespace axon
