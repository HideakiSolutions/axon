#pragma once
#include "db.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace axon {

// A symbol definition located by name, with its body when it fits the budget.
struct SymbolMatch {
    std::string path;
    std::string name;
    std::string kind;
    std::string signature;
    std::string docstring;
    std::string content; // empty when only the signature is returned
    int start_line = 0;
    int end_line = 0;
    int token_estimate = 0;
    bool elided = false; // the middle of the body was dropped to fit the budget
};

// Keeps the first ~60% and last ~30% of `cap_tokens` (estimated as bytes/4) of `lines`, replacing
// the middle with a marker that says how many lines were dropped. Returns the lines joined when
// they already fit. Declarations and return paths survive; an arbitrary byte cut would not.
std::string elide_body(const std::vector<std::string>& lines, int cap_tokens);

// Finds definitions named `name` (the last component of `Class.method` / `ns::name` is used),
// optionally restricted to files whose path ends with `file_filter` and to one `kind`, and
// returns the first ones with their bodies. A single focused read replaces opening a whole file
// after a capsule: typically a few hundred tokens instead of thousands.
std::vector<SymbolMatch> read_symbols(Database& db, const std::filesystem::path& project_root,
                                      const std::string& name, const std::string& file_filter,
                                      const std::string& kind_filter, int token_budget,
                                      int max_matches = 8);

} // namespace axon
