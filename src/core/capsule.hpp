#pragma once
#include "db.hpp"
#include "graph.hpp"
#include "embeddings.hpp"
#include "../parser/parser.hpp"
#include <string>
#include <vector>
#include <filesystem>

namespace axon {

struct CapsuleFile {
    std::string path;
    std::string content;
    bool        is_skeleton   = false;
    int         token_estimate = 0;
};

struct ContextCapsule {
    std::string             query;
    std::vector<CapsuleFile> pivot_files;
    std::vector<CapsuleFile> support_files;
    int                     token_estimate = 0;
    int                     total_files    = 0;
};

inline int estimate_tokens(const std::string& s) {
    return (int)((s.size() + 3) / 4);
}

ContextCapsule assemble_capsule(
    const std::string&           query,
    const std::vector<std::string>& explicit_pivots,  // empty = use query
    Database&                    db,
    EmbeddingModel&              model,
    const DependencyGraph&       graph,
    const std::filesystem::path& project_root,
    int token_budget = 8000);

} // namespace axon
