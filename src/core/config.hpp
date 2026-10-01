#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace axon {

struct ProjectConfig {
    std::string granularity = "file"; // "file" | "symbol"
    bool index_routes = false;
    bool fts_enabled = true;
    int token_budget = 8000;                 // default capsule token budget
    bool telemetry = false;                  // opt-in anonymous telemetry (W5.T06)
    std::string capsule_compression = "off"; // "off" | "body" (Balde A opt-in)
};

struct Config {
    std::filesystem::path project_root;
    std::filesystem::path axon_dir;   // project_root/.axon/
    std::filesystem::path db_path;    // project_root/.axon/index.duckdb
    std::filesystem::path model_path; // path to .gguf embedding model (env override-aware)
    ProjectConfig project_cfg;
};

// Walk up from cwd looking for .git or Cargo.toml / package.json
std::optional<std::filesystem::path>
find_project_root(const std::filesystem::path& start = std::filesystem::current_path());

Config make_config(const std::filesystem::path& root);

// Find the embedding model (searches common locations)
// `index_model_hint` is the identity of the model that built the vectors already in the index
// (see embedding_state_hint). When given, the matching model file is preferred so queries and
// incremental updates keep working against an index built by an older default; an empty hint
// (a fresh index, or a full `axon index`) selects the current default.
std::filesystem::path find_model(const std::filesystem::path& axon_binary_dir,
                                 const std::string& index_model_hint = "");

ProjectConfig load_project_config(const std::filesystem::path& axon_dir);

} // namespace axon
