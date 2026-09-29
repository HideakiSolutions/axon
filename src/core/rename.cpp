#include "rename.hpp"
#include <fstream>
#include <sstream>
#include <regex>
#include <unordered_map>
#include <filesystem>

namespace axon {

static std::string regex_escape(const std::string& s) {
    static const std::regex special(R"([-[\]{}()*+?.,\\^$|#\s])");
    return std::regex_replace(s, special, R"(\$&)");
}

std::vector<RenameEdit> collect_rename_edits(const std::string& file_path,
                                             const std::string& old_name,
                                             const std::string& new_name) {
    std::vector<RenameEdit> edits;
    std::ifstream f(file_path);
    if (!f) return edits;

    // Whole-word regex: \bold_name\b
    std::regex re("\\b" + regex_escape(old_name) + "\\b",
                  std::regex::ECMAScript | std::regex::optimize);

    std::string line;
    int line_no = 0;
    while (std::getline(f, line)) {
        ++line_no;
        if (!std::regex_search(line, re)) continue;
        // Generate one edit per line that contains the name
        std::string replaced = std::regex_replace(line, re, new_name);
        edits.push_back({file_path, line_no, line, replaced});
    }
    return edits;
}

int apply_rename_edits(const std::vector<RenameEdit>& edits) {
    // Group edits by file
    std::unordered_map<std::string, std::vector<const RenameEdit*>> by_file;
    for (const auto& e : edits)
        by_file[e.file_path].push_back(&e);

    int written = 0;
    for (const auto& [path, file_edits] : by_file) {
        std::ifstream in(path);
        if (!in) continue;

        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
        in.close();

        // Apply edits (indexed by line number)
        std::unordered_map<int, std::string> replacements;
        for (const auto* e : file_edits)
            replacements[e->line] = e->new_text;

        std::ofstream out(path);
        if (!out) continue;
        for (int i = 0; i < (int)lines.size(); i++) {
            auto it = replacements.find(i + 1);
            out << (it != replacements.end() ? it->second : lines[i]) << "\n";
        }
        ++written;
    }
    return written;
}

} // namespace axon
