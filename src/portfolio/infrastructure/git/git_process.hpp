#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace axon::portfolio::git {

struct CommandResult {
    int exit_code = -1;
    std::string stdout_text;
    bool output_truncated = false;
};

// Runs the Git executable without a shell. Callers provide only Git arguments;
// the repository root is always passed as the argument following -C.
CommandResult run(const std::filesystem::path& repository_root,
                  const std::vector<std::string>& arguments,
                  std::size_t maximum_output_bytes = 4U * 1024U * 1024U);

} // namespace axon::portfolio::git
