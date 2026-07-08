#include "shell_filter.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

namespace axon {

namespace {

int estimate_tokens_local(const std::string& s) {
    return static_cast<int>((s.size() + 3) / 4);
}

std::string normalize_command(std::string command) {
    std::transform(command.begin(), command.end(), command.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (command == "rg" || command == "grep") return "grep";
    if (command == "git-diff" || command == "diff") return "diff";
    if (command == "logs") return "log";
    if (command == "json") return "json";
    if (command == "tsc" || command == "typescript" || command == "compiler") return "tsc";
    if (command == "test" || command == "tests" || command == "pytest" || command == "vitest" ||
        command == "ctest" || command == "gtest")
        return "test";
    if (command == "package" || command == "packages" || command == "npm" || command == "pnpm" ||
        command == "yarn" || command == "bun")
        return "package";
    if (command == "lint" || command == "linter" || command == "eslint" || command == "ruff" ||
        command == "prettier" || command == "format")
        return "lint";
    if (command == "text" || command == "plain") return "text";
    return command.empty() ? "auto" : command;
}

OutputKind forced_kind_for_command(const std::string& command, const std::string& input) {
    if (command == "diff") return OutputKind::Diff;
    if (command == "log") return OutputKind::Log;
    if (command == "json") return OutputKind::Json;
    if (command == "grep" || command == "text" || command == "tsc" || command == "test" ||
        command == "package" || command == "lint")
        return OutputKind::PlainText;
    return classify_output(input);
}

struct GrepMatch {
    std::string file;
    std::string line_no;
    std::string text;
};

struct LogLine {
    std::string level;
    std::string text;
    std::string normalized;
};

struct TscDiagnostic {
    std::string file;
    std::string line;
    std::string column;
    std::string severity;
    std::string code;
    std::string message;
};

struct TestBlock {
    std::string title;
    std::vector<std::string> lines;
};

struct PackageSummary {
    std::vector<std::string> operations;
    std::vector<std::string> important;
    int add_count = 0;
    int remove_count = 0;
    int change_count = 0;
};

struct LintDiagnostic {
    std::string file;
    std::string line;
    std::string column;
    std::string severity;
    std::string code;
    std::string message;
};

std::string trim_to(const std::string& s, size_t max_len) {
    if (s.size() <= max_len) return s;
    if (max_len <= 12) return s.substr(0, max_len);
    return s.substr(0, max_len - 12) + "… [truncated]";
}

std::string trim_whitespace(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return s.substr(start, end - start);
}

std::string lowercase_copy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

bool parse_grep_line(const std::string& line, GrepMatch& out) {
    size_t first = line.find(':');
    if (first == std::string::npos || first == 0) return false;
    size_t second = line.find(':', first + 1);
    if (second == std::string::npos || second == first + 1) return false;
    for (size_t i = first + 1; i < second; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(line[i]))) return false;
    }
    out.file = line.substr(0, first);
    out.line_no = line.substr(first + 1, second - first - 1);
    out.text = line.substr(second + 1);
    return true;
}

std::string filter_grep_output(const std::string& input, int token_budget) {
    std::istringstream in(input);
    std::string line;
    std::map<std::string, std::vector<GrepMatch>> by_file;
    std::vector<std::string> passthrough;
    int total_matches = 0;

    while (std::getline(in, line)) {
        GrepMatch match;
        if (parse_grep_line(line, match)) {
            by_file[match.file].push_back(std::move(match));
            ++total_matches;
        } else if (!line.empty()) {
            passthrough.push_back(line);
        }
    }

    if (total_matches == 0) return input;

    int per_file_keep = 3;
    if (token_budget > 800) per_file_keep = 6;
    if (token_budget > 1600) per_file_keep = 10;

    auto build_summary = [&](int keep_per_file, size_t max_line_len) {
        std::ostringstream out;
        out << "# axon grep summary: " << total_matches << " matches in " << by_file.size()
            << " files\n";
        int emitted = 0;
        int omitted = 0;
        for (const auto& [file, matches] : by_file) {
            out << "\n## " << file << " (" << matches.size() << " matches)\n";
            int keep = std::min<int>(keep_per_file, static_cast<int>(matches.size()));
            for (int i = 0; i < keep; ++i) {
                out << matches[i].line_no << ": " << trim_to(matches[i].text, max_line_len) << "\n";
                ++emitted;
            }
            if (static_cast<int>(matches.size()) > keep) {
                int skipped = static_cast<int>(matches.size()) - keep;
                omitted += skipped;
                out << "… " << skipped << " more matches in " << file << "\n";
            }
            if (estimate_tokens_local(out.str()) >= token_budget) break;
        }
        if (omitted > 0) out << "\n# omitted " << omitted << " matches after per-file caps\n";
        if (!passthrough.empty()) out << "# ignored " << passthrough.size() << " non-rg lines\n";
        return std::pair<std::string, int>{out.str(), emitted};
    };

    auto [summary, emitted] = build_summary(per_file_keep, 140);
    if (estimate_tokens_local(summary) > token_budget)
        std::tie(summary, emitted) = build_summary(1, 100);
    if (emitted == 0) return input;

    if (estimate_tokens_local(summary) > token_budget) {
        size_t max_bytes = static_cast<size_t>(std::max(token_budget, 1)) * 4;
        std::string marker = "\n# axon grep output truncated to budget\n";
        if (summary.size() > max_bytes) {
            size_t keep = max_bytes > marker.size() ? max_bytes - marker.size() : max_bytes;
            summary = summary.substr(0, keep) + marker;
        }
    }
    return summary;
}

std::string detect_log_level(const std::string& line) {
    std::string lower = lowercase_copy(line);
    if (lower.find("fatal") != std::string::npos || lower.find("panic") != std::string::npos)
        return "fatal";
    if (lower.find("error") != std::string::npos || lower.find("exception") != std::string::npos ||
        lower.find("failed") != std::string::npos || lower.find("failure") != std::string::npos ||
        lower.find("can't ") != std::string::npos || lower.find("cannot ") != std::string::npos)
        return "error";
    if (lower.find("warn") != std::string::npos) return "warn";
    if (lower.find("debug") != std::string::npos) return "debug";
    if (lower.find("trace") != std::string::npos) return "trace";
    if (lower.find("info") != std::string::npos || lower.find("started ") != std::string::npos ||
        lower.find("consumed ") != std::string::npos)
        return "info";
    return "other";
}

bool looks_like_log_line(const std::string& line) {
    if (line.size() >= 15 && std::isupper(static_cast<unsigned char>(line[0])) &&
        std::islower(static_cast<unsigned char>(line[1])) &&
        std::islower(static_cast<unsigned char>(line[2])) &&
        std::isspace(static_cast<unsigned char>(line[3]))) {
        return true;
    }
    if (line.size() >= 19 && std::isdigit(static_cast<unsigned char>(line[0])) &&
        std::isdigit(static_cast<unsigned char>(line[1])) &&
        std::isdigit(static_cast<unsigned char>(line[2])) &&
        std::isdigit(static_cast<unsigned char>(line[3])) && line[4] == '-' && line[7] == '-' &&
        (line[10] == 'T' || line[10] == ' ')) {
        return true;
    }
    return detect_log_level(line) != "other";
}

std::string normalize_log_message(const std::string& line) {
    std::string msg = trim_whitespace(line);

    static const std::regex syslog_prefix(R"(^[A-Z][a-z]{2}\s+\d+\s+\d\d:\d\d:\d\d\s+\S+\s+(.+)$)");
    std::smatch match;
    if (std::regex_match(msg, match, syslog_prefix)) {
        msg = match[1].str();
    } else if (msg.size() >= 20 && std::isdigit(static_cast<unsigned char>(msg[0])) &&
               msg[4] == '-' && msg[7] == '-' && (msg[10] == 'T' || msg[10] == ' ')) {
        size_t first_space = msg.find(' ');
        size_t second_space =
            first_space == std::string::npos ? std::string::npos : msg.find(' ', first_space + 1);
        if (second_space != std::string::npos) msg = msg.substr(second_space + 1);
    }

    size_t colon = msg.find(": ");
    if (colon != std::string::npos && colon < 80) msg = msg.substr(colon + 2);

    msg = std::regex_replace(msg, std::regex(R"(\b\d+\b)"), "#");
    msg = std::regex_replace(msg, std::regex(R"(\s+)"), " ");
    return trim_to(trim_whitespace(msg), 180);
}

std::string filter_log_output(const std::string& input, int token_budget) {
    std::istringstream in(input);
    std::string line;
    std::vector<LogLine> lines;
    std::map<std::string, int> level_counts;
    std::map<std::string, int> repeated;
    int signal_lines = 0;
    int non_empty_lines = 0;

    while (std::getline(in, line)) {
        std::string trimmed = trim_whitespace(line);
        if (trimmed.empty()) continue;
        ++non_empty_lines;
        if (!looks_like_log_line(trimmed)) continue;

        LogLine parsed;
        parsed.level = detect_log_level(trimmed);
        parsed.text = trim_to(trimmed, 220);
        parsed.normalized = normalize_log_message(trimmed);
        level_counts[parsed.level]++;
        repeated[parsed.level + "\t" + parsed.normalized]++;
        if (parsed.level != "other") ++signal_lines;
        lines.push_back(std::move(parsed));
    }

    if (lines.empty() || signal_lines == 0) return input;
    if (lines.size() < 3 || static_cast<int>(lines.size() * 2) < non_empty_lines) {
        return input;
    }

    auto build_summary = [&](int important_keep, int repeat_keep, int edge_keep,
                             size_t max_line_len) {
        std::ostringstream out;
        out << "# axon log summary: " << lines.size() << " log lines";
        if (signal_lines != static_cast<int>(lines.size()))
            out << ", " << signal_lines << " signal lines";
        out << "\n";

        out << "levels:";
        for (const auto& [level, count] : level_counts)
            out << " " << level << "=" << count;
        out << "\n";

        std::vector<std::pair<std::string, int>> repeated_sorted(repeated.begin(), repeated.end());
        std::sort(repeated_sorted.begin(), repeated_sorted.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });

        int emitted = 0;
        if (!repeated_sorted.empty()) {
            out << "\n## repeated messages\n";
            int keep = std::min<int>(repeat_keep, static_cast<int>(repeated_sorted.size()));
            for (int i = 0; i < keep; ++i) {
                if (repeated_sorted[i].second <= 1) break;
                std::string key = repeated_sorted[i].first;
                size_t tab = key.find('\t');
                std::string level = tab == std::string::npos ? "other" : key.substr(0, tab);
                std::string message = tab == std::string::npos ? key : key.substr(tab + 1);
                out << repeated_sorted[i].second << "x [" << level << "] "
                    << trim_to(message, max_line_len) << "\n";
                ++emitted;
            }
        }

        out << "\n## important\n";
        int important = 0;
        std::set<std::string> seen_important;
        for (const auto& parsed : lines) {
            if (parsed.level != "fatal" && parsed.level != "error" && parsed.level != "warn")
                continue;
            std::string key = parsed.level + "\t" + parsed.normalized;
            if (!seen_important.insert(key).second) continue;
            out << "[" << parsed.level << "] " << trim_to(parsed.text, max_line_len) << "\n";
            ++important;
            ++emitted;
            if (important >= important_keep) break;
        }
        if (important == 0) out << "(none)\n";

        out << "\n## edges\n";
        int first_keep = std::min<int>(edge_keep, static_cast<int>(lines.size()));
        for (int i = 0; i < first_keep; ++i) {
            out << "first: [" << lines[i].level << "] " << trim_to(lines[i].text, max_line_len)
                << "\n";
            ++emitted;
        }
        int start = std::max<int>(first_keep, static_cast<int>(lines.size()) - edge_keep);
        for (int i = start; i < static_cast<int>(lines.size()); ++i) {
            out << "last: [" << lines[i].level << "] " << trim_to(lines[i].text, max_line_len)
                << "\n";
            ++emitted;
        }

        return std::pair<std::string, int>{out.str(), emitted};
    };

    auto [summary, emitted] = build_summary(12, 8, 3, 180);
    if (estimate_tokens_local(summary) > token_budget)
        std::tie(summary, emitted) = build_summary(8, 5, 2, 140);
    if (estimate_tokens_local(summary) > token_budget)
        std::tie(summary, emitted) = build_summary(4, 3, 1, 110);
    if (emitted == 0) return input;

    if (estimate_tokens_local(summary) > token_budget) {
        size_t max_bytes = static_cast<size_t>(std::max(token_budget, 1)) * 4;
        std::string marker = "\n# axon log output truncated to budget\n";
        if (summary.size() > max_bytes) {
            size_t keep = max_bytes > marker.size() ? max_bytes - marker.size() : max_bytes;
            summary = summary.substr(0, keep) + marker;
        }
    }
    return summary;
}

std::string json_type_summary(const nlohmann::json& value) {
    if (value.is_object()) return "object(" + std::to_string(value.size()) + " keys)";
    if (value.is_array()) return "array(" + std::to_string(value.size()) + " items)";
    if (value.is_string()) return "string";
    if (value.is_number()) return "number";
    if (value.is_boolean()) return "boolean";
    if (value.is_null()) return "null";
    return "value";
}

std::string filter_json_output(const std::string& input, int token_budget) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(input);
    } catch (const nlohmann::json::parse_error&) {
        return input;
    }

    auto build_summary = [&](int max_depth, int max_entries) {
        std::ostringstream out;
        out << "# axon json summary\n";

        std::function<void(const nlohmann::json&, int, int, const std::string&)> append;
        append = [&](const nlohmann::json& value, int depth, int indent, const std::string& label) {
            std::string pad(static_cast<size_t>(indent), ' ');
            if (depth >= max_depth || (!value.is_object() && !value.is_array())) {
                out << pad << label << json_type_summary(value) << "\n";
                return;
            }

            if (value.is_object()) {
                out << pad << label << "object(" << value.size() << " keys)\n";
                int emitted = 0;
                for (auto it = value.begin(); it != value.end(); ++it) {
                    if (emitted >= max_entries) {
                        out << pad << "  ... " << (value.size() - emitted) << " more keys\n";
                        break;
                    }
                    append(it.value(), depth + 1, indent + 2, it.key() + ": ");
                    ++emitted;
                }
                return;
            }

            out << pad << label << "array(" << value.size() << " items)\n";
            if (!value.empty()) {
                append(value.front(), depth + 1, indent + 2, "items: ");
            }
        };

        append(root, 0, 0, "root: ");
        return out.str();
    };

    std::vector<std::pair<int, int>> attempts = {
        {4, 8},
        {3, 6},
        {2, 4},
        {1, 3},
    };

    std::string summary;
    for (const auto& [depth, entries] : attempts) {
        summary = build_summary(depth, entries);
        if (estimate_tokens_local(summary) <= token_budget) return summary;
    }

    size_t max_bytes = static_cast<size_t>(std::max(token_budget, 1)) * 4;
    std::string marker = "\n# axon json output truncated to budget\n";
    if (summary.size() > max_bytes) {
        size_t keep = max_bytes > marker.size() ? max_bytes - marker.size() : max_bytes;
        summary = summary.substr(0, keep) + marker;
    }
    return summary;
}

bool parse_tsc_line(const std::string& line, TscDiagnostic& out) {
    static const std::regex diagnostic(
        R"(^(.+)\((\d+),(\d+)\):\s+(error|warning)\s+(TS\d+):\s+(.+)$)");
    std::smatch match;
    if (!std::regex_match(line, match, diagnostic)) return false;
    out.file = match[1].str();
    out.line = match[2].str();
    out.column = match[3].str();
    out.severity = match[4].str();
    out.code = match[5].str();
    out.message = match[6].str();
    return true;
}

std::string filter_tsc_output(const std::string& input, int token_budget) {
    std::istringstream in(input);
    std::string line;
    std::map<std::string, std::vector<TscDiagnostic>> by_file;
    std::map<std::string, int> by_code;
    std::vector<std::string> passthrough;
    std::string last_file;
    int total_diagnostics = 0;

    while (std::getline(in, line)) {
        TscDiagnostic diagnostic;
        if (parse_tsc_line(line, diagnostic)) {
            last_file = diagnostic.file;
            by_code[diagnostic.code]++;
            by_file[last_file].push_back(std::move(diagnostic));
            ++total_diagnostics;
        } else if (!last_file.empty() && !line.empty() &&
                   std::isspace(static_cast<unsigned char>(line.front()))) {
            auto& previous = by_file[last_file].back();
            std::string continuation = trim_whitespace(line);
            if (!continuation.empty()) previous.message += " " + continuation;
        } else if (!line.empty()) {
            passthrough.push_back(line);
        }
    }

    if (total_diagnostics == 0) return input;

    auto build_summary = [&](int keep_per_file, size_t max_message_len, int max_files) {
        std::ostringstream out;
        out << "# axon tsc summary: " << total_diagnostics << " diagnostics in " << by_file.size()
            << " files\n";
        out << "codes:";
        for (const auto& [code, count] : by_code) {
            out << " " << code << "=" << count;
        }
        out << "\n";

        int emitted_files = 0;
        int emitted_diagnostics = 0;
        int omitted_diagnostics = 0;
        for (const auto& [file, diagnostics] : by_file) {
            if (emitted_files >= max_files) {
                for (auto it = by_file.find(file); it != by_file.end(); ++it)
                    omitted_diagnostics += static_cast<int>(it->second.size());
                break;
            }
            out << "\n## " << file << " (" << diagnostics.size() << " diagnostics)\n";
            int keep = std::min<int>(keep_per_file, static_cast<int>(diagnostics.size()));
            for (int i = 0; i < keep; ++i) {
                const auto& d = diagnostics[i];
                out << d.line << ":" << d.column << " " << d.code << " "
                    << trim_to(d.message, max_message_len) << "\n";
                ++emitted_diagnostics;
            }
            if (static_cast<int>(diagnostics.size()) > keep) {
                int skipped = static_cast<int>(diagnostics.size()) - keep;
                omitted_diagnostics += skipped;
                out << "... " << skipped << " more diagnostics in " << file << "\n";
            }
            ++emitted_files;
            if (estimate_tokens_local(out.str()) >= token_budget) break;
        }

        if (omitted_diagnostics > 0)
            out << "\n# omitted " << omitted_diagnostics << " diagnostics after caps\n";
        if (!passthrough.empty())
            out << "# ignored " << passthrough.size() << " non-diagnostic lines\n";
        return std::pair<std::string, int>{out.str(), emitted_diagnostics};
    };

    auto [summary, emitted] = build_summary(4, 180, 20);
    if (estimate_tokens_local(summary) > token_budget)
        std::tie(summary, emitted) = build_summary(2, 120, 10);
    if (estimate_tokens_local(summary) > token_budget)
        std::tie(summary, emitted) = build_summary(1, 90, 6);
    if (emitted == 0) return input;

    if (estimate_tokens_local(summary) > token_budget) {
        size_t max_bytes = static_cast<size_t>(std::max(token_budget, 1)) * 4;
        std::string marker = "\n# axon tsc output truncated to budget\n";
        if (summary.size() > max_bytes) {
            size_t keep = max_bytes > marker.size() ? max_bytes - marker.size() : max_bytes;
            summary = summary.substr(0, keep) + marker;
        }
    }
    return summary;
}

bool is_pytest_block_header(const std::string& line) {
    std::string trimmed = trim_whitespace(line);
    if (trimmed.size() < 8) return false;
    size_t start = 0;
    while (start < trimmed.size() &&
           (trimmed[start] == '_' || trimmed[start] == '=' || trimmed[start] == '-')) {
        ++start;
    }
    size_t end = trimmed.size();
    while (end > start &&
           (trimmed[end - 1] == '_' || trimmed[end - 1] == '=' || trimmed[end - 1] == '-')) {
        --end;
    }
    if (start < 3 || trimmed.size() - end < 3 || end <= start) return false;
    std::string title = trim_whitespace(trimmed.substr(start, end - start));
    if (title == "ERRORS" || title == "FAILURES" || title == "short test summary info")
        return false;
    return title.find("ERROR") != std::string::npos || title.find("FAILED") != std::string::npos;
}

bool is_test_summary_line(const std::string& line) {
    return line.find("short test summary info") != std::string::npos ||
           line.find(" failed") != std::string::npos || line.find(" errors") != std::string::npos ||
           line.find(" error") != std::string::npos || line.find(" passed") != std::string::npos ||
           line.find(" tests passed") != std::string::npos ||
           line.find(" tests failed") != std::string::npos ||
           line.find("100% tests passed") != std::string::npos || line.find("FAILED ") == 0 ||
           line.find("ERROR ") == 0 || line.find("[  FAILED  ]") != std::string::npos ||
           line.find("Failed") != std::string::npos;
}

bool is_test_detail_line(const std::string& line) {
    return line.find("Traceback") != std::string::npos ||
           line.find("AssertionError") != std::string::npos ||
           line.find("ImportError") != std::string::npos ||
           line.find("ModuleNotFoundError") != std::string::npos ||
           line.find("Expected") != std::string::npos ||
           line.find("expected") != std::string::npos || line.find("Actual") != std::string::npos ||
           line.find("actual") != std::string::npos || line.rfind("E   ", 0) == 0 ||
           line.rfind("E       ", 0) == 0 || line.find(".cpp:") != std::string::npos ||
           line.find(".cc:") != std::string::npos || line.find(".py:") != std::string::npos ||
           line.find(".ts:") != std::string::npos || line.find(".tsx:") != std::string::npos;
}

std::string normalize_test_header(std::string line) {
    line = trim_whitespace(line);
    while (!line.empty() && (line.front() == '_' || line.front() == '=' || line.front() == '-'))
        line.erase(line.begin());
    while (!line.empty() && (line.back() == '_' || line.back() == '=' || line.back() == '-'))
        line.pop_back();
    return trim_whitespace(line);
}

std::string filter_test_output(const std::string& input, int token_budget) {
    std::istringstream in(input);
    std::string line;
    std::vector<TestBlock> blocks;
    std::vector<std::string> summaries;
    std::vector<std::string> current_lines;
    std::string current_title;
    int total_signal_lines = 0;

    auto flush_block = [&]() {
        if (!current_title.empty() && !current_lines.empty()) {
            blocks.push_back({current_title, current_lines});
        }
        current_title.clear();
        current_lines.clear();
    };

    while (std::getline(in, line)) {
        if (is_pytest_block_header(line)) {
            flush_block();
            current_title = normalize_test_header(line);
            current_lines.push_back(current_title);
            ++total_signal_lines;
            continue;
        }

        if (line.find("[  FAILED  ]") != std::string::npos ||
            line.find("***Failed") != std::string::npos || line.rfind("FAILED ", 0) == 0 ||
            line.rfind("ERROR ", 0) == 0) {
            if (current_title.empty()) current_title = trim_whitespace(line);
            current_lines.push_back(trim_to(trim_whitespace(line), 220));
            ++total_signal_lines;
            continue;
        }

        if (!current_title.empty() && is_test_detail_line(line)) {
            current_lines.push_back(trim_to(trim_whitespace(line), 220));
            ++total_signal_lines;
            continue;
        }

        if (is_test_summary_line(line)) {
            summaries.push_back(trim_to(trim_whitespace(line), 220));
            ++total_signal_lines;
        }
    }
    flush_block();

    if (total_signal_lines == 0) return input;

    auto build_summary = [&](int max_blocks, int max_lines_per_block, size_t max_line_len) {
        std::ostringstream out;
        out << "# axon test summary: " << blocks.size() << " failure/error blocks";
        if (!summaries.empty()) out << ", " << summaries.size() << " summary lines";
        out << "\n";

        int emitted_blocks = 0;
        int emitted_lines = 0;
        for (const auto& block : blocks) {
            if (emitted_blocks >= max_blocks) break;
            out << "\n## " << trim_to(block.title, max_line_len) << "\n";
            int keep = std::min<int>(max_lines_per_block, static_cast<int>(block.lines.size()));
            for (int i = 0; i < keep; ++i) {
                if (i == 0 && block.lines[i] == block.title) continue;
                out << trim_to(block.lines[i], max_line_len) << "\n";
                ++emitted_lines;
            }
            if (static_cast<int>(block.lines.size()) > keep) {
                out << "... " << (static_cast<int>(block.lines.size()) - keep)
                    << " more lines in this failure\n";
            }
            ++emitted_blocks;
            if (estimate_tokens_local(out.str()) >= token_budget) break;
        }

        if (static_cast<int>(blocks.size()) > emitted_blocks) {
            out << "\n# omitted " << (static_cast<int>(blocks.size()) - emitted_blocks)
                << " failure/error blocks after caps\n";
        }

        if (!summaries.empty()) {
            out << "\n## summary\n";
            int keep_summary = std::min<int>(12, static_cast<int>(summaries.size()));
            int start = static_cast<int>(summaries.size()) - keep_summary;
            for (int i = start; i < static_cast<int>(summaries.size()); ++i)
                out << trim_to(summaries[i], max_line_len) << "\n";
            emitted_lines += keep_summary;
        }

        return std::pair<std::string, int>{out.str(), emitted_lines};
    };

    auto [summary, emitted] = build_summary(8, 8, 220);
    if (estimate_tokens_local(summary) > token_budget)
        std::tie(summary, emitted) = build_summary(5, 5, 160);
    if (estimate_tokens_local(summary) > token_budget)
        std::tie(summary, emitted) = build_summary(3, 3, 120);
    if (emitted == 0) return input;

    if (estimate_tokens_local(summary) > token_budget) {
        size_t max_bytes = static_cast<size_t>(std::max(token_budget, 1)) * 4;
        std::string marker = "\n# axon test output truncated to budget\n";
        if (summary.size() > max_bytes) {
            size_t keep = max_bytes > marker.size() ? max_bytes - marker.size() : max_bytes;
            summary = summary.substr(0, keep) + marker;
        }
    }
    return summary;
}

bool starts_with_word(const std::string& line, const std::string& word) {
    if (line.rfind(word, 0) != 0) return false;
    return line.size() == word.size() ||
           std::isspace(static_cast<unsigned char>(line[word.size()]));
}

bool is_package_operation_line(const std::string& line) {
    return starts_with_word(line, "add") || starts_with_word(line, "remove") ||
           starts_with_word(line, "change") || starts_with_word(line, "update");
}

bool is_package_important_line(const std::string& line) {
    std::string trimmed = trim_whitespace(line);
    return trimmed.rfind("npm ERR!", 0) == 0 || trimmed.rfind("npm WARN", 0) == 0 ||
           trimmed.rfind("npm error", 0) == 0 || trimmed.rfind("ERR!", 0) == 0 ||
           trimmed.rfind("WARN", 0) == 0 || trimmed.rfind("warning", 0) == 0 ||
           trimmed.rfind("error", 0) == 0 || trimmed.rfind(">", 0) == 0 ||
           trimmed.rfind("sh: ", 0) == 0 || trimmed.find("deprecated") != std::string::npos ||
           trimmed.find("vulnerabilities") != std::string::npos ||
           trimmed.find("vulnerability") != std::string::npos ||
           trimmed.find("added ") != std::string::npos ||
           trimmed.find("removed ") != std::string::npos ||
           trimmed.find("changed ") != std::string::npos ||
           trimmed.find("updated ") != std::string::npos ||
           trimmed.find("packages are looking for funding") != std::string::npos ||
           trimmed.find("run `npm fund`") != std::string::npos ||
           trimmed.find("audited ") != std::string::npos ||
           trimmed.find("up to date") != std::string::npos ||
           trimmed.find("found 0 vulnerabilities") != std::string::npos;
}

std::string filter_package_output(const std::string& input, int token_budget) {
    std::istringstream in(input);
    std::string line;
    PackageSummary parsed;

    while (std::getline(in, line)) {
        std::string trimmed = trim_whitespace(line);
        if (trimmed.empty()) continue;

        if (is_package_operation_line(trimmed)) {
            parsed.operations.push_back(trim_to(trimmed, 180));
            if (starts_with_word(trimmed, "add")) {
                ++parsed.add_count;
            } else if (starts_with_word(trimmed, "remove")) {
                ++parsed.remove_count;
            } else if (starts_with_word(trimmed, "change") || starts_with_word(trimmed, "update")) {
                ++parsed.change_count;
            }
        } else if (is_package_important_line(trimmed)) {
            parsed.important.push_back(trim_to(trimmed, 220));
        }
    }

    if (parsed.operations.empty() && parsed.important.empty()) return input;

    auto build_summary = [&](int max_ops, int max_important, size_t max_line_len) {
        std::ostringstream out;
        out << "# axon package summary";
        if (!parsed.operations.empty())
            out << ": " << parsed.operations.size() << " package operation lines";
        out << "\n";
        if (parsed.add_count || parsed.remove_count || parsed.change_count) {
            out << "counts:";
            if (parsed.add_count) out << " add=" << parsed.add_count;
            if (parsed.remove_count) out << " remove=" << parsed.remove_count;
            if (parsed.change_count) out << " change=" << parsed.change_count;
            out << "\n";
        }

        int emitted = 0;
        if (!parsed.important.empty()) {
            out << "\n## important\n";
            int keep = std::min<int>(max_important, static_cast<int>(parsed.important.size()));
            for (int i = 0; i < keep; ++i) {
                out << trim_to(parsed.important[i], max_line_len) << "\n";
                ++emitted;
            }
            if (static_cast<int>(parsed.important.size()) > keep)
                out << "... " << (static_cast<int>(parsed.important.size()) - keep)
                    << " more important lines\n";
        }

        if (!parsed.operations.empty()) {
            out << "\n## package operations\n";
            int keep = std::min<int>(max_ops, static_cast<int>(parsed.operations.size()));
            for (int i = 0; i < keep; ++i) {
                out << trim_to(parsed.operations[i], max_line_len) << "\n";
                ++emitted;
            }
            if (static_cast<int>(parsed.operations.size()) > keep)
                out << "... " << (static_cast<int>(parsed.operations.size()) - keep)
                    << " more package operation lines\n";
        }

        return std::pair<std::string, int>{out.str(), emitted};
    };

    auto [summary, emitted] = build_summary(20, 12, 180);
    if (estimate_tokens_local(summary) > token_budget)
        std::tie(summary, emitted) = build_summary(12, 8, 140);
    if (estimate_tokens_local(summary) > token_budget)
        std::tie(summary, emitted) = build_summary(6, 5, 110);
    if (emitted == 0) return input;

    if (estimate_tokens_local(summary) > token_budget) {
        size_t max_bytes = static_cast<size_t>(std::max(token_budget, 1)) * 4;
        std::string marker = "\n# axon package output truncated to budget\n";
        if (summary.size() > max_bytes) {
            size_t keep = max_bytes > marker.size() ? max_bytes - marker.size() : max_bytes;
            summary = summary.substr(0, keep) + marker;
        }
    }
    return summary;
}

bool parse_concise_lint_line(const std::string& line, LintDiagnostic& out) {
    static const std::regex diagnostic(
        R"(^(.+):(\d+):(\d+):\s+([A-Za-z]+[A-Za-z0-9_-]*\d*|[A-Za-z0-9_@/-]+)\s+(.+)$)");
    std::smatch match;
    if (!std::regex_match(line, match, diagnostic)) return false;
    out.file = match[1].str();
    out.line = match[2].str();
    out.column = match[3].str();
    out.code = match[4].str();
    out.message = match[5].str();
    out.severity = "issue";
    return true;
}

bool parse_eslint_stylish_line(const std::string& line, const std::string& current_file,
                               LintDiagnostic& out) {
    if (current_file.empty()) return false;
    static const std::regex diagnostic(
        R"(^\s*(\d+):(\d+)\s+(error|warning)\s+(.+?)\s+([A-Za-z0-9_@./-]+)\s*$)");
    std::smatch match;
    if (!std::regex_match(line, match, diagnostic)) return false;
    out.file = current_file;
    out.line = match[1].str();
    out.column = match[2].str();
    out.severity = match[3].str();
    out.message = match[4].str();
    out.code = match[5].str();
    return true;
}

bool is_lint_summary_line(const std::string& line) {
    std::string trimmed = trim_whitespace(line);
    return trimmed.find("problem") != std::string::npos ||
           trimmed.find("error") != std::string::npos ||
           trimmed.find("warning") != std::string::npos ||
           trimmed.find("Found ") != std::string::npos ||
           trimmed.find("All checks passed") != std::string::npos ||
           trimmed.find("No issues found") != std::string::npos ||
           trimmed.find("files would be reformatted") != std::string::npos ||
           trimmed.find("would reformat") != std::string::npos;
}

std::string filter_lint_output(const std::string& input, int token_budget) {
    std::istringstream in(input);
    std::string line;
    std::string current_file;
    std::map<std::string, std::vector<LintDiagnostic>> by_file;
    std::map<std::string, int> by_code;
    std::vector<std::string> summaries;
    int total_diagnostics = 0;

    while (std::getline(in, line)) {
        std::string trimmed = trim_whitespace(line);
        if (trimmed.empty()) continue;

        LintDiagnostic diagnostic;
        if (parse_concise_lint_line(trimmed, diagnostic) ||
            parse_eslint_stylish_line(line, current_file, diagnostic)) {
            by_code[diagnostic.code]++;
            by_file[diagnostic.file].push_back(std::move(diagnostic));
            ++total_diagnostics;
            continue;
        }

        if (trimmed.find('/') != std::string::npos && trimmed.find(':') == std::string::npos &&
            trimmed.find(' ') == std::string::npos) {
            current_file = trimmed;
            continue;
        }

        if (is_lint_summary_line(trimmed)) {
            summaries.push_back(trim_to(trimmed, 220));
        }
    }

    if (total_diagnostics == 0 && summaries.empty()) return input;

    auto build_summary = [&](int keep_per_file, int max_files, int max_summaries,
                             size_t max_line_len) {
        std::ostringstream out;
        out << "# axon lint summary: " << total_diagnostics << " diagnostics in " << by_file.size()
            << " files\n";
        if (!by_code.empty()) {
            out << "codes:";
            for (const auto& [code, count] : by_code)
                out << " " << code << "=" << count;
            out << "\n";
        }

        int emitted = 0;
        int emitted_files = 0;
        int omitted_diagnostics = 0;
        for (const auto& [file, diagnostics] : by_file) {
            if (emitted_files >= max_files) {
                for (auto it = by_file.find(file); it != by_file.end(); ++it)
                    omitted_diagnostics += static_cast<int>(it->second.size());
                break;
            }
            out << "\n## " << file << " (" << diagnostics.size() << " diagnostics)\n";
            int keep = std::min<int>(keep_per_file, static_cast<int>(diagnostics.size()));
            for (int i = 0; i < keep; ++i) {
                const auto& d = diagnostics[i];
                out << d.line << ":" << d.column << " ";
                if (!d.severity.empty() && d.severity != "issue") out << d.severity << " ";
                out << d.code << " " << trim_to(d.message, max_line_len) << "\n";
                ++emitted;
            }
            if (static_cast<int>(diagnostics.size()) > keep) {
                int skipped = static_cast<int>(diagnostics.size()) - keep;
                omitted_diagnostics += skipped;
                out << "... " << skipped << " more diagnostics in " << file << "\n";
            }
            ++emitted_files;
            if (estimate_tokens_local(out.str()) >= token_budget) break;
        }

        if (omitted_diagnostics > 0)
            out << "\n# omitted " << omitted_diagnostics << " diagnostics after caps\n";

        if (!summaries.empty()) {
            out << "\n## summary\n";
            int keep = std::min<int>(max_summaries, static_cast<int>(summaries.size()));
            int start = static_cast<int>(summaries.size()) - keep;
            for (int i = start; i < static_cast<int>(summaries.size()); ++i) {
                out << trim_to(summaries[i], max_line_len) << "\n";
                ++emitted;
            }
        }

        return std::pair<std::string, int>{out.str(), emitted};
    };

    auto [summary, emitted] = build_summary(5, 12, 8, 180);
    if (estimate_tokens_local(summary) > token_budget)
        std::tie(summary, emitted) = build_summary(3, 8, 6, 140);
    if (estimate_tokens_local(summary) > token_budget)
        std::tie(summary, emitted) = build_summary(1, 5, 4, 110);
    if (emitted == 0) return input;

    if (estimate_tokens_local(summary) > token_budget) {
        size_t max_bytes = static_cast<size_t>(std::max(token_budget, 1)) * 4;
        std::string marker = "\n# axon lint output truncated to budget\n";
        if (summary.size() > max_bytes) {
            size_t keep = max_bytes > marker.size() ? max_bytes - marker.size() : max_bytes;
            summary = summary.substr(0, keep) + marker;
        }
    }
    return summary;
}

} // namespace

ShellFilterResult filter_shell_output(const std::string& command, const std::string& input,
                                      int token_budget) {
    ShellFilterResult result;
    result.command = normalize_command(command);
    result.kind = forced_kind_for_command(result.command, input);
    result.input_tokens = estimate_tokens_local(input);

    if (input.empty() || token_budget <= 0 || result.input_tokens <= token_budget) {
        result.output = input;
        result.output_tokens = result.input_tokens;
        return result;
    }

    std::string filtered;
    if (result.command == "grep") {
        filtered = filter_grep_output(input, token_budget);
    } else if (result.command == "log") {
        filtered = filter_log_output(input, token_budget);
    } else if (result.command == "json") {
        filtered = filter_json_output(input, token_budget);
    } else if (result.command == "tsc") {
        filtered = filter_tsc_output(input, token_budget);
    } else if (result.command == "test") {
        filtered = filter_test_output(input, token_budget);
    } else if (result.command == "package") {
        filtered = filter_package_output(input, token_budget);
    } else if (result.command == "lint") {
        filtered = filter_lint_output(input, token_budget);
    } else {
        filtered = compress_body(input, std::nullopt, token_budget);
    }
    int filtered_tokens = estimate_tokens_local(filtered);
    if (filtered_tokens >= result.input_tokens) {
        result.output = input;
        result.output_tokens = result.input_tokens;
        return result;
    }

    result.output = std::move(filtered);
    result.output_tokens = filtered_tokens;
    result.tokens_saved = result.input_tokens - result.output_tokens;
    result.changed = true;
    return result;
}

} // namespace axon
