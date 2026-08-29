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
    long long owner_started_at = 0;   // Unix epoch seconds
    long long owner_heartbeat_at = 0; // Unix epoch seconds
};

struct RegistryData {
    std::vector<RepoEntry> repos;
    // groups: map name → list of repo names
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;
};

// Returns path to the registry: $AXON_REGISTRY_DIR/registry.json when AXON_REGISTRY_DIR is
// set (tests/sandboxes), else ~/.axon/registry.json (dir created if needed).
std::filesystem::path registry_path();

// Load registry from disk; returns empty RegistryData if file doesn't exist
RegistryData load_registry();

// Save registry to disk
void save_registry(const RegistryData& reg);

// Register (or update) a repo entry in the registry
void register_repo(const std::string& root, const std::string& db_path);

// Record this process as the live DB owner for a repo (pid + peer port + token)
void set_repo_owner(const std::string& root, long long pid, int port, const std::string& token);

// Refresh an owner's heartbeat, but only while the same pid still owns the
// entry. Returns false when ownership has already moved elsewhere.
bool touch_repo_owner(const std::string& root, long long pid);

// Clear owner info for a repo, but only if it still points at `pid`
// (avoids a stale exiting process clobbering a newer owner's registration)
void clear_repo_owner(const std::string& root, long long pid);

// Find a repo entry by root path; returns nullopt if absent
std::optional<RepoEntry> find_repo(const std::string& root);

// Remove entries whose root directory no longer exists and whose registered
// owner process (if any) is dead; group memberships of pruned repos are
// dropped too. Kept entries whose registered owner died (live root, dead
// pid) get their owner fields zeroed. Returns the number of entries removed
// (0 + no dead owners = registry untouched).
int prune_registry();

// How many entries prune_registry() would remove right now. Read-only —
// serve startup prints an advisory from this instead of auto-pruning
// (a temporarily unmounted root must not lose its registration).
int count_prunable(const RegistryData& reg);

// Get all repos
std::vector<RepoEntry> get_repos(const RegistryData& reg);

// Get repos in a group
std::vector<RepoEntry> get_group_repos(const RegistryData& reg, const std::string& group_name);

} // namespace axon
