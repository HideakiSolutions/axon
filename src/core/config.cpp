#include "config.hpp"
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

Config make_config(const fs::path& root) {
    Config cfg;
    cfg.project_root = root;
    cfg.axon_dir = root / ".axon";
    cfg.db_path  = cfg.axon_dir / "index.duckdb";
    fs::create_directories(cfg.axon_dir);
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
