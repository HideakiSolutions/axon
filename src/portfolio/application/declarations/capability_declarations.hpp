#pragma once
#include "portfolio/domain/capability_signature.hpp"
#include <filesystem>
#include <string>
#include <vector>
namespace axon::portfolio {
struct DeclaredCapability { std::string id, normalized_name, bounded_context, source_repository, source_commit, source_path; std::vector<std::string> contracts; };
struct DeclarationImportResult { std::vector<DeclaredCapability> declarations; std::vector<std::string> diagnostics; };
struct CapabilityMatch { std::string observed_id, declared_id; double score = 0; std::vector<std::string> evidence; bool ambiguous = false; };
enum class CapabilityDriftKind { observed_without_declaration, declaration_without_observed, ambiguous_match };
struct CapabilityDrift { CapabilityDriftKind kind; std::string subject_id; std::string detail; };
struct DeclarationComparison { std::vector<CapabilityMatch> matches; std::vector<CapabilityDrift> drift; };
class CapabilityDeclarationImporter { public: DeclarationImportResult import(const std::filesystem::path& root, const std::filesystem::path& relative_fragment) const; };
DeclarationComparison compare_declarations(const std::vector<CapabilitySignature>& observed, const std::vector<DeclaredCapability>& declared);
} // namespace axon::portfolio
