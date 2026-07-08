#pragma once
#include <string>
#include <vector>

namespace axon {

struct RenameEdit {
    std::string file_path;
    int line;             // 1-based
    std::string old_text; // text to replace on that line
    std::string new_text; // replacement text
};

struct RenameResult {
    std::string old_name;
    std::string new_name;
    std::vector<RenameEdit> edits;
    std::string error;
};

// Find all occurrences of old_name in file content (whole-word match) and
// generate RenameEdit entries. Does NOT write to disk.
std::vector<RenameEdit> collect_rename_edits(const std::string& file_path,
                                             const std::string& old_name,
                                             const std::string& new_name);

// Apply edits to disk (write files). Returns number of files written.
int apply_rename_edits(const std::vector<RenameEdit>& edits);

} // namespace axon
