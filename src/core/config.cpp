#include "config.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace axon {

namespace fs = std::filesystem;

static const std::vector<std::string> ROOT_MARKERS = {
    ".git", "Cargo.toml", "package.json", "pyproject.toml", "go.mod", "CMakeLists.txt"
};

std::optional<fs::path> find_project_root(const fs::path& start) {
    fs::path current = fs::absolute(start);
    while (true) {
        for (const auto& marker : ROOT_MARKERS) {
            if (fs::exists(current / marker)) return current;
        }
        auto parent = current.parent_path();
        if (parent == current) return std::nullopt;
        current = parent;
    }
}

ProjectConfig load_project_config(const fs::path& axon_dir) {
    ProjectConfig pcfg;
    fs::path config_path = axon_dir / "config.toml";
    std::ifstream f(config_path);
    if (!f) return pcfg;

    std::string line;
    while (std::getline(f, line)) {
        // Strip leading whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        // Skip comments and lines without '='
        if (line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Trim key and val
        auto trim = [](std::string& s) {
            size_t b = s.find_first_not_of(" \t");
            size_t e = s.find_last_not_of(" \t\r\n");
            s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
        };
        trim(key);
        trim(val);

        if (key == "granularity") {
            // Remove surrounding quotes if present
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
                val = val.substr(1, val.size() - 2);
            if (val == "file" || val == "symbol")
                pcfg.granularity = val;
        } else if (key == "index_routes") {
            pcfg.index_routes = (val == "true");
        } else if (key == "fts_enabled") {
            pcfg.fts_enabled = (val == "true");
        }
        // Unrecognized keys: ignored silently
    }
    return pcfg;
}

Config make_config(const fs::path& root) {
    Config cfg;
    cfg.project_root = root;
    cfg.axon_dir = root / ".axon";
    cfg.db_path  = cfg.axon_dir / "index.duckdb";
    fs::create_directories(cfg.axon_dir);
    cfg.project_cfg = load_project_config(cfg.axon_dir);
    return cfg;
}

fs::path find_model(const fs::path& binary_dir) {
    // Search order: next to binary, then project models/, then ~/.axon/models/
    std::vector<fs::path> candidates = {
        binary_dir / "../models/nomic-embed-text-v1.5.Q4_K_M.gguf",
        binary_dir / "../../models/nomic-embed-text-v1.5.Q4_K_M.gguf",
        fs::path(std::getenv("HOME") ? std::getenv("HOME") : "") / ".axon/models/nomic-embed-text-v1.5.Q4_K_M.gguf",
    };
    for (const auto& p : candidates) {
        if (fs::exists(p)) return fs::canonical(p);
    }
    throw std::runtime_error("Embedding model not found. Download it with:\n"
        "  pip install huggingface_hub\n"
        "  huggingface-cli download nomic-ai/nomic-embed-text-v1.5-GGUF "
        "nomic-embed-text-v1.5.Q4_K_M.gguf --local-dir ./models/");
}

} // namespace axon
