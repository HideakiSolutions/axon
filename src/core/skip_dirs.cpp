// The hard skip-list shared by the indexer walk and the native watchers'
// watch-set pruning. Lives in its own translation unit so the watcher tests
// can link it without dragging in the DuckDB-dependent indexer.
#include "skip_dirs.hpp"

namespace axon {

static const char* const SKIP_DIRS[] = {"node_modules", ".git",  "target", "build",
                                        "__pycache__",  ".axon", "dist",   ".next",
                                        "vendor",       ".venv", "venv",   ".worktrees"};

bool is_hard_skip_dir(const std::string& name) {
    for (const auto* d : SKIP_DIRS)
        if (name == d) return true;
    return false;
}

} // namespace axon
