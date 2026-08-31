#pragma once

#include "portfolio_store.hpp"

#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace axon::portfolio {

// Deterministic reference adapter used to define the behavioral contract shared by real adapters.
// It intentionally has no persistence, transport or infrastructure dependency.
class ReferencePortfolioStore final : public PortfolioStore {
public:
    ProviderCapabilities capabilities() const override;
    ProviderHealth health() const override;
    std::string schema_version() const override;
    std::string protocol_version() const override;
    ApplyResult apply(const RepositoryStreamKey& stream, std::uint64_t expected_cursor,
                      const std::vector<ProjectionEvent>& events) override;
    ReplaceResult replace_repository_stream(const RepositorySnapshot& snapshot,
                                            std::uint64_t expected_cursor) override;
    ApplyResult reidentify_repository_stream(
        const RepositoryReidentification& reidentification,
        std::uint64_t expected_cursor) override;
    CursorEpochManifest stream_state(const RepositoryStreamKey& stream) const override;
    StreamProjection inspect_repository_stream(const RepositoryStreamKey& stream,
                                               std::size_t max_entities) const override;
    MaintenanceResult maintenance(MaintenanceKind kind) override;

private:
    using EntityKey = std::pair<std::string, std::string>;
    struct StreamData {
        CursorEpochManifest state;
        std::map<EntityKey, ProjectionMutation> entities;
        std::map<std::uint64_t, ProjectionEvent> events;
    };
    struct EventReceipt {
        RepositoryStreamKey stream;
        std::uint64_t sequence = 0;
        ProjectionEvent event;
    };

    static void validate_stream(const RepositoryStreamKey& stream);
    static void validate_event(const ProjectionEvent& event, const RepositoryStreamKey& stream,
                               std::uint64_t expected_sequence, std::size_t max_batch_size);
    static std::map<EntityKey, ProjectionMutation>
    snapshot_entities(const RepositorySnapshot& snapshot, std::size_t max_batch_size);

    mutable std::mutex mutex_;
    std::map<RepositoryStreamKey, StreamData> streams_;
    std::map<std::string, EventReceipt> receipts_;
    std::map<std::string, RepositoryReidentification> reidentifications_;
};

} // namespace axon::portfolio
