#pragma once
#include <string>
#include <vector>

namespace axon {

struct FileDiff {
    std::string path;          // relative path from repo root
    struct Hunk {
        int start_line;
        int end_line;
    };
    std::vector<Hunk> hunks;
};

// Run `git diff -U0 <ref>` from project_root and parse hunk headers.
// ref: "HEAD" (staged+unstaged vs HEAD), "HEAD~1" (last commit), etc.
// Returns empty vector if not a git repo or git not available.
std::vector<FileDiff> get_git_diffs(const std::string& project_root, const std::string& ref = "HEAD");

// Check if directory is a git repo
bool is_git_repo(const std::string& path);

} // namespace axon
