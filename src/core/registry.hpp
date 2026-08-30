#pragma once
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <memory>
#include <duckdb.hpp>

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

    // Registry v2 identity. Empty values preserve the v1 contract for legacy
    // registrations until identity is resolved by the portfolio bootstrap.
    std::string repository_id;  // logical source-repository identity
    std::string index_stream_id; // physical index/worktree identity
    std::string variant;
    std::vector<std::string> default_for_profiles;
};

struct ProviderTarget {
    std::string provider;
    std::string path;
    std::string endpoint;
};

struct TargetMarker {
    std::string instance_id;
    std::string namespace_name;
    std::string protocol_version;
};

struct StorageProfile {
    std::string name;
    std::string role;
    std::string transport;
    std::string endpoint;
    std::string namespace_name;
    ProviderTarget portfolio_store;
    std::optional<ProviderTarget> semantic_index;
    std::optional<ProviderTarget> graph_projection;
    std::optional<TargetMarker> target_marker;
    bool is_default = false;
};

struct RegistryValidationIssue {
    std::string code;
    std::string path;
    std::string message;
};

struct RegistryData {
    std::string schema_version;
    std::vector<RegistryValidationIssue> load_issues;
    std::vector<RepoEntry> repos;
    // groups: v2 repository ids; legacy v1 repo names remain migration input.
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;
    std::vector<StorageProfile> storage_profiles;
};

struct RepoSelection {
    std::vector<RepoEntry> repos;
    std::vector<RegistryValidationIssue> issues;
    std::string profile_name;
};

struct ReadOnlySecondary {
    std::unique_ptr<duckdb::DuckDB> db;
    std::string error_code;
    std::string error;

    explicit operator bool() const { return db != nullptr; }
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

// Validate additive registry v2 identity/profile invariants. A legacy v1
// registry (no v2 fields) remains valid and round-trippable.
std::vector<RegistryValidationIssue> validate_registry(const RegistryData& reg);

// Resolve the single default profile and the default physical stream for one
// logical repository. Ambiguous or missing selections return nullopt.
std::optional<StorageProfile> default_storage_profile(const RegistryData& reg);
std::optional<RepoEntry> default_repo_stream(const RegistryData& reg,
                                             const std::string& repository_id,
                                             const std::string& profile_name);

// Resolve repositories for an aggregate query. V1 entries retain name-based
// behavior. V2 registries fail closed on validation errors and select only the
// default physical stream for the default storage profile.
RepoSelection aggregation_repos(const RegistryData& reg,
                                const std::optional<std::string>& group_name = std::nullopt);

// Open a registered project index without any write capability. The opener
// rejects missing targets, symlinks and paths escaping the registered root and
// reports a stable error code suitable for MCP/HTTP responses.
ReadOnlySecondary open_secondary_read_only(const RepoEntry& repo);

} // namespace axon
