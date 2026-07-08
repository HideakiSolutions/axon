#pragma once
#include <string>

namespace axon {

// True for directory names the indexer never descends into (node_modules,
// .git, build, ...). Native watchers prune their watch-set with the same
// predicate so both layers agree on what "the project" is. Defined in
// skip_dirs.cpp — a DuckDB-free translation unit the watcher tests can link.
bool is_hard_skip_dir(const std::string& name);

} // namespace axon
