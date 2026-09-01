#pragma once

#include "portfolio/domain/capability_signature.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace axon::portfolio {

struct GraphTraversal {
    std::vector<std::string> capability_ids;
    bool truncated = false;
};

class CapabilityGraphProjection {
public:
    virtual ~CapabilityGraphProjection() = default;
    virtual void replace_repository(const RepositoryStreamKey& stream,
                                    const std::string& generation,
                                    const std::vector<CapabilitySignature>& signatures) = 0;
    virtual GraphTraversal traverse(const RepositoryStreamKey& stream,
                                    const std::string& signature_id, std::size_t max_depth,
                                    std::size_t max_nodes) const = 0;
};

std::string graph_capability_id(const RepositoryStreamKey& stream, const std::string& signature_id);

} // namespace axon::portfolio
