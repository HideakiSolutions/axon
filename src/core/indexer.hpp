#pragma once
#include "config.hpp"
#include "db.hpp"
#include <filesystem>
#include <functional>
#include <vector>

namespace axon {

struct IndexStats {
    int files_indexed   = 0;
    int files_skipped   = 0;
    int symbols_found   = 0;
    int edges_found     = 0;
    int files_pruned    = 0;
};

using ProgressCallback = std::function<void(const std::string& current_file, int done, int total)>;

IndexStats index_project(
    const Config& cfg,
    Database& db,
    ProgressCallback on_progress = nullptr);

// Incrementally index only the given files (no filesystem walk).
// Paths may be absolute or relative to cfg.project_root; both are accepted.
// When prune=true, after processing paths, the function also scans every row in
// `files` and removes rows whose on-disk path no longer exists (plus their
// symbols and edges). Passing an empty `paths` with prune=true means
// "only perform the delete-sweep".
IndexStats index_files(
    const Config& cfg,
    Database& db,
    const std::vector<std::filesystem::path>& paths,
    bool prune = false,
    ProgressCallback on_progress = nullptr);

// Full walk + upsert + sweep. Cheap in steady-state thanks to the BLAKE3
// skip inside upsert_file (unchanged files cost only a hash + DB lookup).
// Use this when something opaque happened to the filesystem — git checkout,
// rename, external delete, script-driven generation — and the index needs
// to converge with disk in a single pass.
IndexStats sync_project(
    const Config& cfg,
    Database& db,
    ProgressCallback on_progress = nullptr);

} // namespace axon
