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
std::filesystem::path find_model(const std::filesystem::path& axon_binary_dir);

ProjectConfig load_project_config(const std::filesystem::path& axon_dir);

} // namespace axon
