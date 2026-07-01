#pragma once

#include "db.hpp"
#include <optional>
#include <string>

namespace axon {

struct CcrArtifact {
    std::string artifact_id;
    std::string kind;
    std::string source_ref;
    std::string content;
    int64_t     token_estimate = 0;
};

struct CcrRecoverableOutput {
    bool        recoverable = false;
    std::string output;
    std::string artifact_id;
    int64_t     input_tokens = 0;
    int64_t     output_tokens = 0;
    int64_t     tokens_saved = 0;
};

std::string ccr_artifact_id(const std::string& kind,
                            const std::string& source_ref,
                            const std::string& content);

std::string ccr_marker(const std::string& artifact_id,
                       int64_t original_tokens);

std::string ccr_store_artifact(Database& db,
                               const std::string& kind,
                               const std::string& source_ref,
                               const std::string& content,
                               int64_t token_estimate);

std::optional<CcrArtifact> ccr_retrieve_artifact(Database& db,
                                                 const std::string& artifact_id);

CcrRecoverableOutput ccr_make_recoverable_output(Database& db,
                                                 const std::string& kind,
                                                 const std::string& source_ref,
                                                 const std::string& original,
                                                 const std::string& lossy_output,
                                                 int64_t original_tokens);

} // namespace axon
