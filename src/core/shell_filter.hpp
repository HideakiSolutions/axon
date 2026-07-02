#pragma once

#include "compress.hpp"
#include <string>

namespace axon {

struct ShellFilterResult {
    std::string command;
    OutputKind  kind = OutputKind::PlainText;
    std::string output;
    int         input_tokens = 0;
    int         output_tokens = 0;
    int         tokens_saved = 0;
    bool        changed = false;
};

ShellFilterResult filter_shell_output(const std::string& command,
                                      const std::string& input,
                                      int token_budget);

} // namespace axon
