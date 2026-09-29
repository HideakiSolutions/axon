#include "semantic_capability_store.hpp"
#include <cmath>
namespace axon::portfolio {
void validate_semantic_identity(const SemanticIdentity& identity,
                                const std::vector<float>& vector) {
    if (identity.model_id.empty() || identity.generation.empty() || identity.dimension == 0 ||
        identity.dimension > 65536 || vector.size() != identity.dimension ||
        identity.metric != "cosine")
        throw std::invalid_argument("semantic identity/vector mismatch");
    for (const auto value : vector)
        if (!std::isfinite(value)) throw std::invalid_argument("semantic vector must be finite");
}
} // namespace axon::portfolio
