#pragma once

#include "portfolio/domain/capability_signature.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace axon::portfolio {

struct SemanticIdentity {
    std::string model_id;
    std::uint32_t dimension = 0;
    std::string metric;
    std::string generation;
    bool operator==(const SemanticIdentity&) const = default;
};
struct SemanticRecord {
    std::string signature_id;
    std::string repository_id;
    std::string index_epoch;
    SemanticIdentity identity;
    std::vector<float> vector;
};
struct SemanticHit {
    std::string signature_id;
    float score = 0;
};

class SemanticCapabilityStore {
public:
    virtual ~SemanticCapabilityStore() = default;
    virtual void upsert(const SemanticRecord&) = 0;
    virtual void erase(const std::string& signature_id, const std::string& generation) = 0;
    virtual std::vector<SemanticHit> search(const std::vector<float>& query,
                                            const SemanticIdentity&, std::size_t limit) const = 0;
};

void validate_semantic_identity(const SemanticIdentity&, const std::vector<float>&);
} // namespace axon::portfolio
