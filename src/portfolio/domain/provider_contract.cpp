#include "provider_contract.hpp"

#include <algorithm>
#include <tuple>

namespace axon::portfolio {

bool ProviderCapabilities::supports(ProviderCapability capability) const {
    return std::find(supported.begin(), supported.end(), capability) != supported.end();
}

bool RepositoryStreamKey::operator<(const RepositoryStreamKey& other) const {
    return std::tie(repository_id, index_stream_id) <
           std::tie(other.repository_id, other.index_stream_id);
}

PortfolioStoreError::PortfolioStoreError(PortfolioStoreErrorCode code,
                                         const std::string& message)
    : std::runtime_error(message), code_(code) {}

} // namespace axon::portfolio
