#pragma once
#include "../parser/parser.hpp"
#include <string>

namespace axon {

// Returns a signatures-only view of a source file.
// Functions/methods: docstring + signature line only (body stripped).
// Classes: header + method signatures.
// Imports/types: kept in full.
std::string skeletonize(const std::string& source, Language lang, bool strip_comments = true);

} // namespace axon
