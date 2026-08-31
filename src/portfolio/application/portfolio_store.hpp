#pragma once

#include "portfolio/domain/provider_contract.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace axon::portfolio {

class PortfolioStore {
public:
    virtual ~PortfolioStore() = default;

    virtual ProviderCapabilities capabilities() const = 0;
    virtual ProviderHealth health() const = 0;
    virtual std::string schema_version() const = 0;
    virtual std::string protocol_version() const = 0;
    virtual ApplyResult apply(const RepositoryStreamKey& stream, std::uint64_t expected_cursor,
                              const std::vector<ProjectionEvent>& events) = 0;
    virtual ReplaceResult replace_repository_stream(const RepositorySnapshot& snapshot,
                                                    std::uint64_t expected_cursor) = 0;
    virtual CursorEpochManifest stream_state(const RepositoryStreamKey& stream) const = 0;
    // Bounded projected metadata inspection makes delete/replace atomicity independently
    // conformable across adapters without exposing source bodies or infrastructure handles.
    virtual StreamProjection inspect_repository_stream(const RepositoryStreamKey& stream,
                                                       std::size_t max_entities) const = 0;
    virtual MaintenanceResult maintenance(MaintenanceKind kind) = 0;
};

} // namespace axon::portfolio
