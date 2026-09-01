#include "portfolio/application/declarations/capability_declarations.hpp"
#include "portfolio/infrastructure/git/git_process.hpp"
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace axon::portfolio {
namespace {
constexpr std::size_t kMaximumDeclarationBytes = 4U * 1024U * 1024U;

void trim(std::string& value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
}

bool is_object_id(const std::string& value) {
    return (value.size() == 40U || value.size() == 64U) &&
           std::all_of(value.begin(), value.end(),
                       [](const unsigned char c) { return std::isxdigit(c); });
}

std::string verified_head(const std::filesystem::path& root) {
    auto result = git::run(root, {"rev-parse", "--verify", "HEAD"}, 128U);
    trim(result.stdout_text);
    if (result.exit_code != 0 || result.output_truncated || !is_object_id(result.stdout_text)) {
        throw std::invalid_argument("unable to verify immutable Git provenance");
    }
    return result.stdout_text;
}

std::filesystem::path checked_relative_path(const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory()) {
        throw std::invalid_argument("fragment path must be root-contained");
    }
    const auto normalized = relative.lexically_normal();
    for (const auto& component : normalized) {
        if (component == ".." || component == "." ||
            component.string().find(':') != std::string::npos) {
            throw std::invalid_argument("fragment path must be root-contained");
        }
    }
    return normalized;
}

void require_regular_git_blob(const std::filesystem::path& root, const std::string& sha,
                              const std::filesystem::path& relative) {
    const auto listing =
        git::run(root, {"ls-tree", "-z", sha, "--", relative.generic_string()}, 4096U);
    const auto expected_prefix = std::string{"100644 blob "};
    const auto executable_prefix = std::string{"100755 blob "};
    if (listing.exit_code != 0 || listing.output_truncated ||
        (listing.stdout_text.rfind(expected_prefix, 0) != 0 &&
         listing.stdout_text.rfind(executable_prefix, 0) != 0)) {
        throw std::invalid_argument("declaration fragment is not a regular Git blob");
    }
}
} // namespace

DeclarationImportResult
CapabilityDeclarationImporter::import(const std::filesystem::path& root,
                                      const std::filesystem::path& relative) const {
    const auto canonical = std::filesystem::canonical(root);
    if (!std::filesystem::is_directory(canonical))
        throw std::invalid_argument("declaration root is unavailable");
    const auto safe_relative = checked_relative_path(relative);
    const auto sha = verified_head(canonical);
    require_regular_git_blob(canonical, sha, safe_relative);
    const auto content = git::run(
        canonical,
        {"show", "--no-ext-diff", "--no-textconv", sha + ":" + safe_relative.generic_string()},
        kMaximumDeclarationBytes);
    if (content.exit_code != 0 || content.output_truncated) {
        throw std::invalid_argument("immutable declaration fragment is unavailable");
    }
    const auto json = nlohmann::json::parse(content.stdout_text);
    if (json.value("schema_version", "") != "axon/capability-graph/v1" ||
        !json.contains("capabilities") || !json["capabilities"].is_array())
        throw std::invalid_argument("unsupported declaration schema");
    DeclarationImportResult out;
    for (const auto& item : json["capabilities"]) {
        if (!item.is_object() || !item.contains("id") || !item.contains("name") ||
            !item["id"].is_string() || !item["name"].is_string())
            throw std::invalid_argument("invalid capability declaration");
        DeclaredCapability declaration;
        declaration.id = item["id"];
        declaration.normalized_name = normalize_capability_name(item["name"]);
        declaration.bounded_context = item.value("bounded_context", "");
        declaration.contracts = item.value("contracts", std::vector<std::string>{});
        declaration.source_repository = canonical.filename().string();
        declaration.source_commit = sha;
        declaration.source_path = safe_relative.generic_string();
        if (declaration.id.empty() || declaration.normalized_name.empty() ||
            declaration.contracts.size() > 2000U) {
            throw std::invalid_argument("invalid capability declaration bounds");
        }
        out.declarations.push_back(std::move(declaration));
    }
    return out;
}
} // namespace axon::portfolio
