#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace axon {

struct RepoEntry {
    std::string name;       // repo name (last component of root path)
    std::string root;       // absolute path to repo root
    std::string db_path;    // absolute path to .axon/index.duckdb
};

struct RegistryData {
    std::vector<RepoEntry> repos;
    // groups: map name → list of repo names
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;
};

// Returns path to ~/.axon/registry.json (creates ~/.axon dir if needed)
std::filesystem::path registry_path();

// Load registry from disk; returns empty RegistryData if file doesn't exist
RegistryData load_registry();

// Save registry to disk
void save_registry(const RegistryData& reg);

// Register (or update) a repo entry in the registry
void register_repo(const std::string& root, const std::string& db_path);

// Get all repos
std::vector<RepoEntry> get_repos(const RegistryData& reg);

// Get repos in a group
std::vector<RepoEntry> get_group_repos(const RegistryData& reg, const std::string& group_name);

} // namespace axon
