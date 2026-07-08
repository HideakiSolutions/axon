#include "registry.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
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

std::filesystem::path registry_path() {
    // AXON_HOME overrides the default ~/.axon — tests and sandboxes point it
    // at a scratch dir so they never touch the user's real registry.
    std::filesystem::path axon_dir;
    const char* axon_home = getenv("AXON_HOME");
    if (axon_home && *axon_home) {
        axon_dir = axon_home;
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
        return reg;
    }

    if (j.contains("repos") && j["repos"].is_array()) {
        for (const auto& r : j["repos"]) {
            RepoEntry entry;
            entry.name = r.value("name", "");
            entry.root = r.value("root", "");
            entry.db_path = r.value("db_path", "");
            entry.owner_pid = r.value("owner_pid", 0LL);
            entry.owner_port = r.value("owner_port", 0);
            entry.owner_token = r.value("owner_token", "");
            if (!entry.root.empty()) reg.repos.push_back(entry);
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

    return reg;
}

void save_registry(const RegistryData& reg) {
    json j;
    j["repos"] = json::array();
    for (const auto& r : reg.repos) {
        json entry = {{"name", r.name}, {"root", r.root}, {"db_path", r.db_path}};
        if (r.owner_pid != 0) {
            entry["owner_pid"] = r.owner_pid;
            entry["owner_port"] = r.owner_port;
            entry["owner_token"] = r.owner_token;
        }
        j["repos"].push_back(entry);
    }
    j["groups"] = json::object();
    for (const auto& [gname, members] : reg.groups) {
        j["groups"][gname] = members;
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
    for (auto& r : reg.repos) {
        if (r.root != root) continue;
        r.owner_pid = pid;
        r.owner_port = port;
        r.owner_token = token;
        save_registry(reg);
        return;
    }
}

void clear_repo_owner(const std::string& root, long long pid) {
    auto reg = load_registry();
    for (auto& r : reg.repos) {
        if (r.root != root || r.owner_pid != pid) continue;
        r.owner_pid = 0;
        r.owner_port = 0;
        r.owner_token.clear();
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

int prune_registry() {
    auto reg = load_registry();
    std::vector<RepoEntry> kept;
    kept.reserve(reg.repos.size());
    int removed = 0;
    for (auto& r : reg.repos) {
        bool root_exists = std::filesystem::exists(r.root);
        bool owner_alive = r.owner_pid != 0 && process_alive(r.owner_pid);
        if (root_exists || owner_alive) {
            kept.push_back(std::move(r));
        } else {
            removed++;
        }
    }
    if (removed == 0) return 0;

    std::unordered_set<std::string> kept_names;
    for (const auto& r : kept)
        kept_names.insert(r.name);
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
        for (const auto& member : members) {
            for (const auto& r : reg.repos) {
                if (r.name == member) {
                    result.push_back(r);
                    break;
                }
            }
        }
        return result;
    }
    return {};
}

} // namespace axon
