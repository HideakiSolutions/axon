#include "git.hpp"
#include <cstdio>
#include <sstream>
#include <regex>
#include <filesystem>

namespace axon {

bool is_git_repo(const std::string& path) {
    namespace fs = std::filesystem;
    return fs::exists(fs::path(path) / ".git");
}

std::vector<FileDiff> get_git_diffs(const std::string& project_root, const std::string& ref) {
    // Build command: git diff -U0 <ref> from project_root
    std::string cmd = "git -C '" + project_root + "' diff -U0 " + ref + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};

    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) output += buf;
    pclose(pipe);

    if (output.empty()) {
        // Try staged changes too
        cmd = "git -C '" + project_root + "' diff -U0 --cached " + ref + " 2>/dev/null";
        pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buf, sizeof(buf), pipe)) output += buf;
            pclose(pipe);
        }
    }

    std::vector<FileDiff> diffs;
    FileDiff* current = nullptr;

    std::istringstream ss(output);
    std::string line;
    // Regex for diff --git a/path b/path
    std::regex file_re(R"(^diff --git a/(.+) b/.+$)");
    // Regex for @@ -old +new,count @@ or @@ -old +new @@
    std::regex hunk_re(R"(^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@)");

    while (std::getline(ss, line)) {
        std::smatch m;
        if (std::regex_match(line, m, file_re)) {
            diffs.push_back({m[1].str(), {}});
            current = &diffs.back();
        } else if (current && std::regex_search(line, m, hunk_re)) {
            int start = std::stoi(m[1].str());
            int count = (m[2].length() > 0) ? std::stoi(m[2].str()) : 1;
            if (count == 0) continue; // pure deletion hunk
            current->hunks.push_back({start, start + count - 1});
        }
    }
    return diffs;
}

} // namespace axon
