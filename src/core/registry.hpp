#pragma once
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace axon {

struct RepoEntry {
    std::string name;    // repo name (last component of root path)
    std::string root;    // absolute path to repo root
    std::string db_path; // absolute path to .axon/index.duckdb
    // Live-owner info: the process currently holding the DuckDB write lock.
    // Latecomer serves proxy their tool calls to this endpoint instead of
    // failing with a lock error. Zeroed when no owner is registered.
    long long owner_pid = 0;
    int owner_port = 0;
    std::string owner_token;
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

// Record this process as the live DB owner for a repo (pid + peer port + token)
void set_repo_owner(const std::string& root, long long pid, int port, const std::string& token);

// Clear owner info for a repo, but only if it still points at `pid`
// (avoids a stale exiting process clobbering a newer owner's registration)
void clear_repo_owner(const std::string& root, long long pid);

// Find a repo entry by root path; returns nullopt if absent
std::optional<RepoEntry> find_repo(const std::string& root);

// Get all repos
std::vector<RepoEntry> get_repos(const RegistryData& reg);

// Get repos in a group
std::vector<RepoEntry> get_group_repos(const RegistryData& reg, const std::string& group_name);

} // namespace axon
