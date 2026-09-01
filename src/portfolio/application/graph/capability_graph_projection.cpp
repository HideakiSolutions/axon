#include "capability_graph_projection.hpp"

#include <stdexcept>

namespace axon::portfolio {

std::string graph_capability_id(const RepositoryStreamKey& stream,
                                const std::string& signature_id) {
    if (stream.repository_id.empty() || stream.index_stream_id.empty() || signature_id.empty()) {
        throw std::invalid_argument("graph capability identity is required");
    }
    return stream.repository_id + ":" + stream.index_stream_id + ":" + signature_id;
}

} // namespace axon::portfolio
