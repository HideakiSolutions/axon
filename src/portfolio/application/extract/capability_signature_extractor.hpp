#pragma once

#include "portfolio/domain/capability_signature.hpp"

#include <optional>
#include <string>
#include <vector>

namespace axon::portfolio {

struct CapabilityExtractionRequest {
    RepositoryStreamKey stream;
    std::uint64_t source_sequence = 0;
    std::string index_epoch;
    std::string manifest_hash;
    std::string extracted_at;
    std::optional<std::string> bounded_context;
    std::string configuration_digest;
    std::vector<CapabilityInput> entities;
    // An empty set means full snapshot extraction. Otherwise only these local entity keys are
    // recomputed, which makes journal-driven extraction partition incremental.
    std::vector<std::string> affected_entity_keys;
    bool incremental = false;
};

class CapabilitySignatureExtractor {
public:
    std::vector<CapabilitySignature> extract(const CapabilityExtractionRequest& request) const;
};

} // namespace axon::portfolio
