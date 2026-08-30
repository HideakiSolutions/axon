#include "registry.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define axon_getpid _getpid
#else
#include <csignal>
#include <unistd.h>
#define axon_getpid getpid
#endif

namespace axon {

using json = nlohmann::json;

namespace {

ProviderTarget provider_from_json(const json& value) {
    ProviderTarget target;
    if (!value.is_object()) return target;
    target.provider = value.value("provider", "");
    target.path = value.value("path", "");
    target.endpoint = value.value("endpoint", "");
    return target;
}

bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& child) {
    auto root_it = root.begin();
    auto child_it = child.begin();
    for (; root_it != root.end(); ++root_it, ++child_it) {
        if (child_it == child.end() || *root_it != *child_it) return false;
    }
    return child_it != child.end();
}

bool contains_symlink(const std::filesystem::path& root, const std::filesystem::path& child) {
    std::error_code ec;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(root, ec))) return true;
    auto absolute_root = std::filesystem::absolute(root, ec).lexically_normal();
    if (ec) return true;
    auto absolute_child = std::filesystem::absolute(child, ec).lexically_normal();
    if (ec || !path_is_within(absolute_root, absolute_child)) return false;
    auto relative = absolute_child.lexically_relative(absolute_root);
    if (relative.empty()) return true;
    auto cursor = absolute_root;
    for (const auto& part : relative) {
        cursor /= part;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(cursor, ec)) || ec)
            return true;
    }
    return false;
}

bool is_uuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    return true;
}

bool valid_shared_endpoint(const std::string& endpoint) {
    const bool tls = endpoint.rfind("https://", 0) == 0;
    const bool plaintext = endpoint.rfind("http://", 0) == 0;
    if (!tls && !plaintext) return false;
    const size_t scheme_end = endpoint.find("//") + 2;
    const size_t authority_end = endpoint.find_first_of("/?#", scheme_end);
    const std::string authority = endpoint.substr(scheme_end, authority_end - scheme_end);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    if (tls) return true;
    return authority == "localhost" || authority.rfind("localhost:", 0) == 0 ||
           authority == "127.0.0.1" || authority.rfind("127.0.0.1:", 0) == 0 ||
           authority == "[::1]" || authority.rfind("[::1]:", 0) == 0;
}

void add_type_issue(std::vector<RegistryValidationIssue>& issues, const std::string& path,
                    const std::string& expected) {
    issues.push_back({"invalid_registry_type", path, "expected " + expected});
}

void validate_string_field(const json& object, const char* field, const std::string& path,
                           std::vector<RegistryValidationIssue>& issues) {
    if (object.contains(field) && !object[field].is_string())
        add_type_issue(issues, path + "." + field, "string");
}

void validate_integer_field(const json& object, const char* field, const std::string& path,
                            std::vector<RegistryValidationIssue>& issues) {
    if (object.contains(field) && !object[field].is_number_integer())
        add_type_issue(issues, path + "." + field, "integer");
}

void validate_provider_object(const json& value, const std::string& path,
                              std::vector<RegistryValidationIssue>& issues) {
    if (!value.is_object()) {
        add_type_issue(issues, path, "object");
        return;
    }
    validate_string_field(value, "provider", path, issues);
    validate_string_field(value, "path", path, issues);
    validate_string_field(value, "endpoint", path, issues);
}

std::vector<RegistryValidationIssue> validate_registry_json_shape(const json& document) {
    std::vector<RegistryValidationIssue> issues;
    if (!document.is_object()) {
        add_type_issue(issues, "$", "object");
        return issues;
    }

    validate_string_field(document, "schema_version", "$", issues);

    if (document.contains("repos")) {
        const auto& repos = document["repos"];
        if (!repos.is_array()) {
            add_type_issue(issues, "$.repos", "array");
        } else {
            for (size_t i = 0; i < repos.size(); ++i) {
                const auto& repo = repos[i];
                const std::string path = "$.repos[" + std::to_string(i) + "]";
                if (!repo.is_object()) {
                    add_type_issue(issues, path, "object");
                    continue;
                }
                for (const char* field : {"name", "root", "db_path", "owner_token",
                                          "repository_id", "index_stream_id", "variant"})
                    validate_string_field(repo, field, path, issues);
                for (const char* field : {"owner_pid", "owner_port", "owner_started_at",
                                          "owner_heartbeat_at"})
                    validate_integer_field(repo, field, path, issues);
                if (repo.contains("default_for_profiles")) {
                    const auto& defaults = repo["default_for_profiles"];
                    if (!defaults.is_array()) {
                        add_type_issue(issues, path + ".default_for_profiles", "array");
                    } else {
                        for (size_t j = 0; j < defaults.size(); ++j)
                            if (!defaults[j].is_string())
                                add_type_issue(issues, path + ".default_for_profiles[" +
                                                           std::to_string(j) + "]",
                                               "string");
                    }
                }
            }
        }
    }

    if (document.contains("groups")) {
        const auto& groups = document["groups"];
        if (!groups.is_object()) {
            add_type_issue(issues, "$.groups", "object");
        } else {
            for (const auto& [name, members] : groups.items()) {
                const std::string path = "$.groups." + name;
                if (!members.is_array()) {
                    add_type_issue(issues, path, "array");
                    continue;
                }
                for (size_t i = 0; i < members.size(); ++i)
                    if (!members[i].is_string())
                        add_type_issue(issues, path + "[" + std::to_string(i) + "]", "string");
            }
        }
    }

    if (document.contains("storage_profiles")) {
        const auto& profiles = document["storage_profiles"];
        if (!profiles.is_object()) {
            add_type_issue(issues, "$.storage_profiles", "object");
        } else {
            for (const auto& [name, profile] : profiles.items()) {
                const std::string path = "$.storage_profiles." + name;
                if (!profile.is_object()) {
                    add_type_issue(issues, path, "object");
                    continue;
                }
                for (const char* field : {"role", "transport", "endpoint", "namespace",
                                          "provider", "path"})
                    validate_string_field(profile, field, path, issues);
                if (profile.contains("default") && !profile["default"].is_boolean())
                    add_type_issue(issues, path + ".default", "boolean");
                for (const char* field : {"portfolio_store", "semantic_index",
                                          "graph_projection"})
                    if (profile.contains(field))
                        validate_provider_object(profile[field], path + "." + field, issues);
                if (profile.contains("providers")) {
                    const auto& providers = profile["providers"];
                    if (!providers.is_object()) {
                        add_type_issue(issues, path + ".providers", "object");
                    } else {
                        for (const auto& [role, provider] : providers.items())
                            if (!provider.is_string())
                                add_type_issue(issues, path + ".providers." + role, "string");
                    }
                }
                if (profile.contains("target_marker")) {
                    const auto& marker = profile["target_marker"];
                    if (!marker.is_object()) {
                        add_type_issue(issues, path + ".target_marker", "object");
                    } else {
                        for (const char* field : {"instance_id", "namespace",
                                                  "protocol_version"})
                            validate_string_field(marker, field, path + ".target_marker", issues);
                    }
                }
            }
        }
    }
    return issues;
}

} // namespace

std::filesystem::path registry_path() {
    // AXON_REGISTRY_DIR overrides the default ~/.axon — tests and sandboxes
    // point it at a scratch dir so they never touch the user's real registry.
    // (Not AXON_HOME: the install wrappers already use that name to relocate
    // the package root, so reusing it here would break every installed CLI.)
    std::filesystem::path axon_dir;
    const char* reg_dir = getenv("AXON_REGISTRY_DIR");
    if (reg_dir && *reg_dir) {
        axon_dir = reg_dir;
    } else {
        const char* home = getenv("HOME");
        if (!home) home = "/tmp";
        axon_dir = std::filesystem::path(home) / ".axon";
    }
    std::filesystem::create_directories(axon_dir);
    return axon_dir / "registry.json";
}

RegistryData load_registry() {
    RegistryData reg;
    auto path = registry_path();
    if (!std::filesystem::exists(path)) return reg;

    std::ifstream f(path);
    if (!f) return reg;

    json j;
    try {
        f >> j;
    } catch (...) {
        reg.load_issues.push_back(
            {"invalid_registry_json", "$", "registry.json is not valid JSON"});
        return reg;
    }

    reg.load_issues = validate_registry_json_shape(j);
    if (!reg.load_issues.empty()) return reg;

    try {
        reg.schema_version = j.value("schema_version", "");

    if (j.contains("repos") && j["repos"].is_array()) {
        for (const auto& r : j["repos"]) {
            RepoEntry entry;
            entry.name = r.value("name", "");
            entry.root = r.value("root", "");
            entry.db_path = r.value("db_path", "");
            entry.owner_pid = r.value("owner_pid", 0LL);
            entry.owner_port = r.value("owner_port", 0);
            entry.owner_token = r.value("owner_token", "");
            entry.owner_started_at = r.value("owner_started_at", 0LL);
            entry.owner_heartbeat_at = r.value("owner_heartbeat_at", 0LL);
            entry.repository_id = r.value("repository_id", "");
            entry.index_stream_id = r.value("index_stream_id", "");
            entry.variant = r.value("variant", "");
            if (r.contains("default_for_profiles") && r["default_for_profiles"].is_array()) {
                for (const auto& profile : r["default_for_profiles"])
                    if (profile.is_string())
                        entry.default_for_profiles.push_back(profile.get<std::string>());
            }
            if (!entry.root.empty()) reg.repos.push_back(entry);
        }
    }

    if (j.contains("storage_profiles") && j["storage_profiles"].is_object()) {
        for (const auto& [name, value] : j["storage_profiles"].items()) {
            if (!value.is_object()) continue;
            StorageProfile profile;
            profile.name = name;
            profile.role = value.value("role", "");
            profile.transport = value.value("transport", "");
            profile.endpoint = value.value("endpoint", "");
            profile.namespace_name = value.value("namespace", "");
            profile.is_default = value.value("default", false);
            if (value.contains("portfolio_store"))
                profile.portfolio_store = provider_from_json(value["portfolio_store"]);
            profile.portfolio_store.provider =
                value.value("provider", profile.portfolio_store.provider);
            profile.portfolio_store.path = value.value("path", profile.portfolio_store.path);
            if (value.contains("providers") && value["providers"].is_object()) {
                const auto& providers = value["providers"];
                profile.portfolio_store.provider =
                    providers.value("portfolio_store", profile.portfolio_store.provider);
                if (providers.contains("semantic_index") &&
                    providers["semantic_index"].is_string())
                    profile.semantic_index =
                        ProviderTarget{providers["semantic_index"].get<std::string>(), "", ""};
                if (providers.contains("graph_projection") &&
                    providers["graph_projection"].is_string())
                    profile.graph_projection =
                        ProviderTarget{providers["graph_projection"].get<std::string>(), "", ""};
            }
            if (value.contains("semantic_index"))
                profile.semantic_index = provider_from_json(value["semantic_index"]);
            if (value.contains("graph_projection"))
                profile.graph_projection = provider_from_json(value["graph_projection"]);
            if (value.contains("target_marker") && value["target_marker"].is_object()) {
                const auto& marker = value["target_marker"];
                profile.target_marker = TargetMarker{marker.value("instance_id", ""),
                                                     marker.value("namespace", ""),
                                                     marker.value("protocol_version", "")};
            }
            reg.storage_profiles.push_back(std::move(profile));
        }
    }

    if (j.contains("groups") && j["groups"].is_object()) {
        for (auto& [gname, members] : j["groups"].items()) {
            std::vector<std::string> repos;
            if (members.is_array())
                for (const auto& m : members)
                    repos.push_back(m.get<std::string>());
            reg.groups.emplace_back(gname, std::move(repos));
        }
    }
    } catch (const json::exception&) {
        RegistryData invalid;
        invalid.load_issues.push_back(
            {"invalid_registry_type", "$", "registry.json contains a field with an invalid type"});
        return invalid;
    }

    return reg;
}

void save_registry(const RegistryData& reg) {
    if (!reg.load_issues.empty()) return;
    json j;
    j["repos"] = json::array();
    for (const auto& r : reg.repos) {
        json entry = {{"name", r.name}, {"root", r.root}, {"db_path", r.db_path}};
        if (r.owner_pid != 0) {
            entry["owner_pid"] = r.owner_pid;
            entry["owner_port"] = r.owner_port;
            entry["owner_token"] = r.owner_token;
            entry["owner_started_at"] = r.owner_started_at;
            entry["owner_heartbeat_at"] = r.owner_heartbeat_at;
        }
        if (!r.repository_id.empty()) entry["repository_id"] = r.repository_id;
        if (!r.index_stream_id.empty()) entry["index_stream_id"] = r.index_stream_id;
        if (!r.variant.empty()) entry["variant"] = r.variant;
        if (!r.default_for_profiles.empty())
            entry["default_for_profiles"] = r.default_for_profiles;
        j["repos"].push_back(entry);
    }
    j["groups"] = json::object();
    for (const auto& [gname, members] : reg.groups) {
        j["groups"][gname] = members;
    }
    bool has_v2_repos = std::any_of(reg.repos.begin(), reg.repos.end(), [](const auto& repo) {
        return !repo.repository_id.empty() || !repo.index_stream_id.empty() ||
               !repo.variant.empty() || !repo.default_for_profiles.empty();
    });
    if (reg.schema_version == "axon-registry/v2" || !reg.storage_profiles.empty() || has_v2_repos) {
        j["schema_version"] = "axon-registry/v2";
        j["storage_profiles"] = json::object();
        for (const auto& profile : reg.storage_profiles) {
            json value = {{"role", profile.role}, {"default", profile.is_default}};
            if (profile.role == "portfolio_local") {
                value["provider"] = profile.portfolio_store.provider;
                value["path"] = profile.portfolio_store.path;
            } else {
                value["providers"] = {{"portfolio_store", profile.portfolio_store.provider}};
                if (profile.semantic_index)
                    value["providers"]["semantic_index"] = profile.semantic_index->provider;
                if (profile.graph_projection)
                    value["providers"]["graph_projection"] = profile.graph_projection->provider;
            }
            if (!profile.transport.empty()) value["transport"] = profile.transport;
            if (!profile.endpoint.empty()) value["endpoint"] = profile.endpoint;
            if (!profile.namespace_name.empty()) value["namespace"] = profile.namespace_name;
            if (profile.target_marker) {
                value["target_marker"] = {{"instance_id", profile.target_marker->instance_id},
                                           {"namespace", profile.target_marker->namespace_name},
                                           {"protocol_version",
                                            profile.target_marker->protocol_version}};
            }
            j["storage_profiles"][profile.name] = std::move(value);
        }
    }

    // Write-to-temp + rename so concurrent readers never see a half-written
    // registry (multiple serves upsert owner info independently). The temp
    // name is pid-unique so two processes saving at once cannot interleave
    // writes into the same temp file.
    auto path = registry_path();
    auto tmp = path;
    tmp += ".new." + std::to_string(axon_getpid());
    {
        std::ofstream f(tmp);
        f << j.dump(2) << "\n";
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::ofstream f(path);
        f << j.dump(2) << "\n";
    }
}

void register_repo(const std::string& root, const std::string& db_path) {
    auto reg = load_registry();
    if (!reg.load_issues.empty()) return;
    std::string name = std::filesystem::path(root).filename().string();

    // Upsert: find existing by root
    for (auto& r : reg.repos) {
        if (r.root == root) {
            r.name = name;
            r.db_path = db_path;
            save_registry(reg);
            return;
        }
    }

    RepoEntry entry;
    entry.name = name;
    entry.root = root;
    entry.db_path = db_path;
    reg.repos.push_back(entry);
    save_registry(reg);
}

void set_repo_owner(const std::string& root, long long pid, int port, const std::string& token) {
    auto reg = load_registry();
    if (!reg.load_issues.empty()) return;
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    for (auto& r : reg.repos) {
        if (r.root != root) continue;
        if (r.owner_pid != pid || r.owner_token != token) r.owner_started_at = now;
        r.owner_pid = pid;
        r.owner_port = port;
        r.owner_token = token;
        r.owner_heartbeat_at = now;
        save_registry(reg);
        return;
    }
}

bool touch_repo_owner(const std::string& root, long long pid) {
    auto reg = load_registry();
    if (!reg.load_issues.empty()) return false;
    for (auto& r : reg.repos) {
        if (r.root != root || r.owner_pid != pid) continue;
        r.owner_heartbeat_at = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
        save_registry(reg);
        return true;
    }
    return false;
}

void clear_repo_owner(const std::string& root, long long pid) {
    auto reg = load_registry();
    if (!reg.load_issues.empty()) return;
    for (auto& r : reg.repos) {
        if (r.root != root || r.owner_pid != pid) continue;
        r.owner_pid = 0;
        r.owner_port = 0;
        r.owner_token.clear();
        r.owner_started_at = 0;
        r.owner_heartbeat_at = 0;
        save_registry(reg);
        return;
    }
}

std::optional<RepoEntry> find_repo(const std::string& root) {
    auto reg = load_registry();
    for (const auto& r : reg.repos)
        if (r.root == root) return r;
    return std::nullopt;
}

std::vector<RepoEntry> get_repos(const RegistryData& reg) {
    return reg.repos;
}

// Local liveness probe (peer.cpp has its own for lock-owner handoff; core/
// must not depend on mcp/, so the ~10 lines are duplicated here on purpose).
static bool process_alive(long long pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return ::kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}

int count_prunable(const RegistryData& reg) {
    int dead = 0;
    for (const auto& r : reg.repos) {
        bool root_exists = std::filesystem::exists(r.root);
        bool owner_alive = r.owner_pid != 0 && process_alive(r.owner_pid);
        if (!root_exists && !owner_alive) dead++;
    }
    return dead;
}

int prune_registry() {
    auto reg = load_registry();
    if (!reg.load_issues.empty()) return 0;
    std::vector<RepoEntry> kept;
    kept.reserve(reg.repos.size());
    int removed = 0;
    int owners_cleared = 0;
    for (auto& r : reg.repos) {
        bool root_exists = std::filesystem::exists(r.root);
        bool owner_alive = r.owner_pid != 0 && process_alive(r.owner_pid);
        if (root_exists || owner_alive) {
            // A dead owner on a live root is stale bookkeeping (the process
            // exited without clear_repo_owner — crash, SIGKILL): zero it so
            // status/peers don't chase a gone endpoint. Serve takeover also
            // handles this lazily; prune just makes the registry honest now.
            if (r.owner_pid != 0 && !owner_alive) {
                r.owner_pid = 0;
                r.owner_port = 0;
                r.owner_token.clear();
                r.owner_started_at = 0;
                r.owner_heartbeat_at = 0;
                owners_cleared++;
            }
            kept.push_back(std::move(r));
        } else {
            removed++;
        }
    }
    if (removed == 0 && owners_cleared > 0) {
        reg.repos = std::move(kept);
        save_registry(reg);
        return 0;
    }
    if (removed == 0) return 0;

    std::unordered_set<std::string> kept_names;
    for (const auto& r : kept) {
        kept_names.insert(r.name);
        if (!r.repository_id.empty()) kept_names.insert(r.repository_id);
    }
    for (auto& [gname, members] : reg.groups) {
        members.erase(std::remove_if(members.begin(), members.end(),
                                     [&](const std::string& m) { return !kept_names.count(m); }),
                      members.end());
    }

    reg.repos = std::move(kept);
    save_registry(reg);
    return removed;
}

std::vector<RepoEntry> get_group_repos(const RegistryData& reg, const std::string& group_name) {
    for (const auto& [gname, members] : reg.groups) {
        if (gname != group_name) continue;
        std::vector<RepoEntry> result;
        std::unordered_set<std::string> selected;
        for (const auto& member : members) {
            for (const auto& r : reg.repos) {
                if (r.name == member || (!r.repository_id.empty() && r.repository_id == member)) {
                    const std::string key =
                        r.index_stream_id.empty() ? r.root : r.index_stream_id;
                    if (selected.insert(key).second) result.push_back(r);
                }
            }
        }
        return result;
    }
    return {};
}

std::vector<RegistryValidationIssue> validate_registry(const RegistryData& reg) {
    std::vector<RegistryValidationIssue> issues = reg.load_issues;
    auto add = [&](std::string code, std::string path, std::string message) {
        issues.push_back({std::move(code), std::move(path), std::move(message)});
    };

    std::unordered_map<std::string, std::string> stream_owner;
    std::unordered_map<std::string, int> default_streams;
    std::unordered_set<std::string> logical_repositories;
    for (size_t i = 0; i < reg.repos.size(); ++i) {
        const auto& repo = reg.repos[i];
        const std::string base = "repos[" + std::to_string(i) + "]";
        const bool has_v2_identity = !repo.repository_id.empty() || !repo.index_stream_id.empty() ||
                                     !repo.default_for_profiles.empty();
        if (has_v2_identity && (repo.repository_id.empty() || repo.index_stream_id.empty()))
            add("incomplete_stream_identity", base,
                "repository_id and index_stream_id must be supplied together");
        if (!repo.repository_id.empty() && !is_uuid(repo.repository_id))
            add("invalid_repository_id", base + ".repository_id",
                "repository_id must be a canonical UUID");
        if (!repo.repository_id.empty()) logical_repositories.insert(repo.repository_id);
        if (!repo.index_stream_id.empty() && !is_uuid(repo.index_stream_id))
            add("invalid_index_stream_id", base + ".index_stream_id",
                "index_stream_id must be a canonical UUID");
        if (!repo.index_stream_id.empty()) {
            auto [it, inserted] = stream_owner.emplace(repo.index_stream_id, repo.root);
            if (!inserted)
                add("duplicate_stream_binding", base + ".index_stream_id",
                    "index_stream_id is already bound to " + it->second);
        }
        std::unordered_set<std::string> seen_profiles;
        for (const auto& profile : repo.default_for_profiles) {
            if (!seen_profiles.insert(profile).second)
                add("duplicate_profile_default", base + ".default_for_profiles",
                    "profile is repeated for the same stream");
            default_streams[repo.repository_id + "\n" + profile]++;
        }
    }

    std::unordered_map<std::string, int> defaults_by_role;
    std::unordered_set<std::string> profile_names;
    for (size_t i = 0; i < reg.storage_profiles.size(); ++i) {
        const auto& profile = reg.storage_profiles[i];
        const std::string base = "storage_profiles." + profile.name;
        if (profile.name.empty() || !profile_names.insert(profile.name).second)
            add("duplicate_profile", base, "profile names must be non-empty and unique");
        if (profile.role != "portfolio_local" && profile.role != "portfolio_shared")
            add("invalid_profile_role", base + ".role",
                "expected portfolio_local or portfolio_shared");
        if (profile.is_default) defaults_by_role[profile.role]++;
        if (profile.role == "portfolio_local" && !profile.transport.empty() &&
            profile.transport != "local")
            add("invalid_transport", base + ".transport",
                "portfolio_local transport may be omitted or local");
        if (profile.role == "portfolio_shared" && profile.transport != "axon_http")
            add("invalid_transport", base + ".transport",
                "portfolio_shared transport must be axon_http");
        if (profile.portfolio_store.provider.empty())
            add("missing_role_provider", base + ".portfolio_store.provider",
                "portfolio_store provider is required");
        if (profile.role == "portfolio_local" && profile.portfolio_store.provider != "duckdb")
            add("invalid_local_store", base + ".portfolio_store.provider",
                "local profile requires duckdb");
        if (profile.role == "portfolio_local" &&
            (profile.portfolio_store.path.empty() || !profile.endpoint.empty() ||
             !profile.portfolio_store.endpoint.empty()))
            add("invalid_local_target", base,
                "portfolio_local requires path and prohibits endpoint");
        if (profile.role == "portfolio_shared" &&
            (!profile.portfolio_store.path.empty() || profile.endpoint.empty()))
            add("invalid_shared_target", base,
                "portfolio_shared requires endpoint and prohibits path");
        if (profile.role == "portfolio_shared" && !valid_shared_endpoint(profile.endpoint))
            add("invalid_shared_endpoint", base + ".endpoint",
                "shared endpoint requires HTTPS, except HTTP on explicit loopback hosts");
        if (profile.role == "portfolio_shared" &&
            profile.portfolio_store.provider != "postgresql")
            add("invalid_shared_store", base + ".portfolio_store.provider",
                "axon_http profile requires postgresql for the durable portfolio store");
        if (profile.semantic_index && profile.semantic_index->provider != "qdrant")
            add("invalid_semantic_provider", base + ".semantic_index.provider",
                "semantic_index role accepts qdrant");
        if (profile.graph_projection && profile.graph_projection->provider != "falkordb")
            add("invalid_graph_provider", base + ".graph_projection.provider",
                "graph_projection role accepts falkordb");
        if (profile.role == "portfolio_shared") {
            if (profile.endpoint.empty())
                add("missing_endpoint", base + ".endpoint", "axon_http endpoint is required");
            if (profile.namespace_name.empty())
                add("missing_namespace", base + ".namespace", "axon_http namespace is required");
            if (!profile.target_marker)
                add("missing_target_marker", base + ".target_marker",
                    "remote profiles require a target marker");
        }
        if (profile.target_marker &&
            (profile.target_marker->instance_id.empty() ||
             profile.target_marker->protocol_version != "axon/portfolio-sync/v1" ||
             profile.target_marker->namespace_name != profile.namespace_name))
            add("target_marker_mismatch", base + ".target_marker",
                "marker must identify an instance/protocol and match the profile namespace");
    }
    for (const auto& [role, count] : defaults_by_role) {
        if (count > 1)
            add("ambiguous_default_profile", "storage_profiles",
                "at most one storage profile may be the default for role " + role);
    }
    auto default_count = std::count_if(reg.storage_profiles.begin(), reg.storage_profiles.end(),
                                       [](const auto& profile) { return profile.is_default; });
    if (!reg.storage_profiles.empty() && default_count != 1)
        add("invalid_operational_default", "storage_profiles",
            "exactly one profile must be the operational default");
    if ((!reg.storage_profiles.empty() || !logical_repositories.empty()) &&
        reg.schema_version != "axon-registry/v2")
        add("invalid_schema_version", "schema_version",
            "registry v2 fields require axon-registry/v2");
    if (!reg.schema_version.empty() && reg.schema_version != "axon-registry/v2")
        add("unsupported_schema_version", "schema_version",
            "supported registry versions are legacy v1 (omitted) and axon-registry/v2");
    for (const auto& [binding, count] : default_streams) {
        if (count != 1)
            add("ambiguous_default_stream", "repos.default_for_profiles",
                "exactly one stream may be default for repository/profile " + binding);
    }
    for (const auto& repository_id : logical_repositories) {
        for (const auto& profile : reg.storage_profiles) {
            const std::string binding = repository_id + "\n" + profile.name;
            if (!default_streams.count(binding))
                add("missing_default_stream", "repos.default_for_profiles",
                    "no default stream for repository/profile " + binding);
        }
    }
    for (const auto& repo : reg.repos) {
        for (const auto& profile : repo.default_for_profiles) {
            if (!profile_names.count(profile))
                add("unknown_stream_profile", "repos.default_for_profiles",
                    "default stream references unknown profile " + profile);
        }
    }
    return issues;
}

std::optional<StorageProfile> default_storage_profile(const RegistryData& reg) {
    std::optional<StorageProfile> result;
    for (const auto& profile : reg.storage_profiles) {
        if (!profile.is_default) continue;
        if (result) return std::nullopt;
        result = profile;
    }
    return result;
}

std::optional<RepoEntry> default_repo_stream(const RegistryData& reg,
                                             const std::string& repository_id,
                                             const std::string& profile_name) {
    std::optional<RepoEntry> result;
    for (const auto& repo : reg.repos) {
        if (repo.repository_id != repository_id ||
            std::find(repo.default_for_profiles.begin(), repo.default_for_profiles.end(),
                      profile_name) == repo.default_for_profiles.end())
            continue;
        if (result) return std::nullopt;
        result = repo;
    }
    return result;
}

RepoSelection aggregation_repos(const RegistryData& reg,
                                const std::optional<std::string>& group_name) {
    RepoSelection selection;
    selection.issues = validate_registry(reg);
    if (!selection.issues.empty()) return selection;

    auto candidates = group_name ? get_group_repos(reg, *group_name) : get_repos(reg);
    if (reg.schema_version.empty()) {
        selection.repos = std::move(candidates);
        return selection;
    }
    auto profile = default_storage_profile(reg);
    if (!profile) {
        selection.issues.push_back({"missing_default_profile", "storage_profiles",
                                    "no unambiguous default storage profile"});
        return selection;
    }
    selection.profile_name = profile->name;
    for (const auto& repo : candidates) {
        if (std::find(repo.default_for_profiles.begin(), repo.default_for_profiles.end(),
                      profile->name) != repo.default_for_profiles.end())
            selection.repos.push_back(repo);
    }
    return selection;
}

ReadOnlySecondary open_secondary_read_only(const RepoEntry& repo) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (repo.root.empty() || repo.db_path.empty())
        return {nullptr, "invalid_registration", "registered root and db_path are required"};
    fs::path root(repo.root);
    fs::path db_path(repo.db_path);
    if (!fs::is_directory(root, ec) || ec)
        return {nullptr, "root_unavailable", "registered root is unavailable"};
    if (!fs::is_regular_file(db_path, ec) || ec)
        return {nullptr, "database_unavailable", "registered database is unavailable"};
    if (contains_symlink(root, db_path))
        return {nullptr, "symlink_rejected", "secondary path contains a symbolic link"};
    auto canonical_root = fs::canonical(root, ec);
    if (ec) return {nullptr, "root_unavailable", "registered root cannot be canonicalized"};
    auto canonical_db = fs::canonical(db_path, ec);
    if (ec || !path_is_within(canonical_root, canonical_db))
        return {nullptr, "path_outside_root", "database must be contained by registered root"};
    try {
        duckdb::DBConfig config;
        config.options.access_mode = duckdb::AccessMode::READ_ONLY;
        return {std::make_unique<duckdb::DuckDB>(canonical_db.string(), &config), "", ""};
    } catch (const std::exception& e) {
        return {nullptr, "database_open_failed", e.what()};
    } catch (...) {
        return {nullptr, "database_open_failed", "unknown DuckDB open failure"};
    }
}

} // namespace axon
