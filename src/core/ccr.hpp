#pragma once

#include "db.hpp"
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace axon {

struct CcrArtifact {
    std::string artifact_id;
    std::string kind;
    std::string source_ref;
    std::string content;
    int64_t token_estimate = 0;
};

struct CcrRecoverableOutput {
    bool recoverable = false;
    std::string output;
    std::string artifact_id;
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t tokens_saved = 0;
};

std::string ccr_artifact_id(const std::string& kind, const std::string& source_ref,
                            const std::string& content);

std::string ccr_marker(const std::string& artifact_id, int64_t original_tokens);

std::string ccr_store_artifact(Database& db, const std::string& kind, const std::string& source_ref,
                               const std::string& content, int64_t token_estimate);

std::optional<CcrArtifact> ccr_retrieve_artifact(Database& db, const std::string& artifact_id);

// File-backed sidecar store under <axon_dir>/ccr/, one JSON file per
// artifact. Used when the DuckDB index cannot be opened (typically another
// axon process holds the write lock), so `axon filter` keeps producing
// recoverable output instead of silently passing input through.
std::string ccr_store_artifact_file(const std::filesystem::path& ccr_dir, const std::string& kind,
                                    const std::string& source_ref, const std::string& content,
                                    int64_t token_estimate);

std::optional<CcrArtifact> ccr_retrieve_artifact_file(const std::filesystem::path& ccr_dir,
                                                      const std::string& artifact_id);

// Store callback: (kind, source_ref, content, token_estimate) -> artifact_id
// ("" on failure). Lets the recoverable-output logic run against either the
// DuckDB table or the file sidecar.
using CcrStoreFn =
    std::function<std::string(const std::string&, const std::string&, const std::string&, int64_t)>;

CcrRecoverableOutput ccr_make_recoverable_output(const CcrStoreFn& store, const std::string& kind,
                                                 const std::string& source_ref,
                                                 const std::string& original,
                                                 const std::string& lossy_output,
                                                 int64_t original_tokens);

CcrRecoverableOutput ccr_make_recoverable_output(Database& db, const std::string& kind,
                                                 const std::string& source_ref,
                                                 const std::string& original,
                                                 const std::string& lossy_output,
                                                 int64_t original_tokens);

} // namespace axon
