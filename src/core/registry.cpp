#include "registry.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdlib>

namespace axon {

using json = nlohmann::json;

std::filesystem::path registry_path() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    std::filesystem::path axon_dir = std::filesystem::path(home) / ".axon";
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
    try { f >> j; } catch (...) { return reg; }

    if (j.contains("repos") && j["repos"].is_array()) {
        for (const auto& r : j["repos"]) {
            RepoEntry entry;
            entry.name    = r.value("name", "");
            entry.root    = r.value("root", "");
            entry.db_path = r.value("db_path", "");
            if (!entry.root.empty()) reg.repos.push_back(entry);
        }
    }

    if (j.contains("groups") && j["groups"].is_object()) {
        for (auto& [gname, members] : j["groups"].items()) {
            std::vector<std::string> repos;
            if (members.is_array())
                for (const auto& m : members) repos.push_back(m.get<std::string>());
            reg.groups.emplace_back(gname, std::move(repos));
        }
    }

    return reg;
}

void save_registry(const RegistryData& reg) {
    json j;
    j["repos"] = json::array();
    for (const auto& r : reg.repos) {
        j["repos"].push_back({{"name", r.name}, {"root", r.root}, {"db_path", r.db_path}});
    }
    j["groups"] = json::object();
    for (const auto& [gname, members] : reg.groups) {
        j["groups"][gname] = members;
    }

    auto path = registry_path();
    std::ofstream f(path);
    f << j.dump(2) << "\n";
}

void register_repo(const std::string& root, const std::string& db_path) {
    auto reg = load_registry();
    std::string name = std::filesystem::path(root).filename().string();

    // Upsert: find existing by root
    for (auto& r : reg.repos) {
        if (r.root == root) {
            r.name    = name;
            r.db_path = db_path;
            save_registry(reg);
            return;
        }
    }

    reg.repos.push_back({name, root, db_path});
    save_registry(reg);
}

std::vector<RepoEntry> get_repos(const RegistryData& reg) {
    return reg.repos;
}

std::vector<RepoEntry> get_group_repos(const RegistryData& reg, const std::string& group_name) {
    for (const auto& [gname, members] : reg.groups) {
        if (gname != group_name) continue;
        std::vector<RepoEntry> result;
        for (const auto& member : members) {
            for (const auto& r : reg.repos) {
                if (r.name == member) { result.push_back(r); break; }
            }
        }
        return result;
    }
    return {};
}

} // namespace axon
