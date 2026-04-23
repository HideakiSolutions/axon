#pragma once
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace axon {

enum class Language { TypeScript, JavaScript, Python, Rust, Go, CSharp, PHP, Dart, Java, Bash, Cpp, Kotlin, Vue };

std::optional<Language> language_from_extension(const std::string& ext);
std::string language_name(Language lang);

struct Symbol {
    std::string name;
    std::string kind;       // function|class|method|type|interface|variable
    int start_line = 0;
    int end_line   = 0;
    int start_byte = 0;
    int end_byte   = 0;
    std::optional<std::string> signature;
    std::optional<std::string> docstring;
};

struct ImportEdge {
    std::string from_path;
    std::string to_specifier;  // raw import string (resolved later)
    std::string kind;          // imports|extends|uses
};

struct ParsedFile {
    std::string path;          // relative to project root
    Language language;
    std::string hash;          // blake3 hex
    std::vector<Symbol> symbols;
    std::vector<ImportEdge> imports;
};

// Returns nullopt if language is unsupported or file unreadable
std::optional<ParsedFile> parse_file(
    const std::filesystem::path& abs_path,
    const std::filesystem::path& project_root);

} // namespace axon
