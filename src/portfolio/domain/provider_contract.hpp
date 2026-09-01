#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace axon::portfolio {

enum class ProviderCapability {
    AtomicApply,
    ReplaceRepositoryStream,
    ReidentifyRepositoryStream,
    ValidateMaintenance,
    CompactMaintenance,
    RebuildDerivedMaintenance,
    CapabilityQueries,
    CandidateQueries,
    DriftQueries,
};

enum class ProviderRole { PortfolioStore, SemanticIndex, GraphProjection };

struct ProviderCapabilities {
    ProviderRole role = ProviderRole::PortfolioStore;
    std::vector<ProviderCapability> supported;
    std::size_t max_batch_size = 0;
    // max_batch_size is retained as the v1 alias for max_events_per_batch.
    std::size_t max_events_per_batch = 0;
    std::size_t max_mutations_per_event = 0;
    std::size_t max_snapshot_entities = 0;

    bool supports(ProviderCapability capability) const;
};

enum class ProviderHealthStatus { Healthy, Degraded, Unavailable };

struct ProviderHealth {
    ProviderHealthStatus status = ProviderHealthStatus::Unavailable;
    std::string detail;
};

struct RepositoryStreamKey {
    std::string repository_id;
    std::string index_stream_id;

    bool operator==(const RepositoryStreamKey&) const = default;
    bool operator<(const RepositoryStreamKey& other) const;
};

struct CursorEpochManifest {
    bool exists = false;
    std::uint64_t cursor = 0;
    std::string epoch;
    std::string manifest;
    bool stale = false;
    bool removed = false;

    bool operator==(const CursorEpochManifest&) const = default;
};

enum class ProjectionOperation { Upsert, Delete };

struct ProjectionMutation {
    std::string entity_kind;
    std::string entity_key;
    ProjectionOperation operation = ProjectionOperation::Upsert;
    std::optional<std::string> digest;

    bool operator==(const ProjectionMutation&) const = default;
};

struct ProjectionEvent {
    RepositoryStreamKey stream;
    std::uint64_t sequence = 0;
    std::string event_id;
    std::string epoch;
    // Present only when the event carries a verified manifest (mandatory for snapshots in the
    // wire schema). Incremental deltas advance epoch/cursor while preserving the last verified one.
    std::optional<std::string> manifest;
    std::vector<ProjectionMutation> mutations;

    bool operator==(const ProjectionEvent&) const = default;
};

struct RepositorySnapshot {
    RepositoryStreamKey stream;
    std::uint64_t cursor = 0;
    std::string epoch;
    std::string manifest;
    bool stale = false;
    bool removed = false;
    std::vector<ProjectionMutation> entities;
};

struct RepositoryReidentification {
    RepositoryStreamKey previous_stream;
    RepositoryStreamKey current_stream;
    std::uint64_t sequence = 0;
    std::string event_id;
    std::string epoch;
    std::optional<std::string> manifest;
    std::string old_binding_id;
    std::string new_binding_id;
    std::string approval_reference;
    std::string reason;

    bool operator==(const RepositoryReidentification&) const = default;
};

struct StreamProjection {
    CursorEpochManifest state;
    std::vector<ProjectionMutation> entities;
    bool truncated = false;
};

enum class ApplyDisposition { Applied, Duplicate };

struct ApplyResult {
    ApplyDisposition disposition = ApplyDisposition::Applied;
    CursorEpochManifest state;
    std::size_t mutations_applied = 0;
};

using ReplaceResult = ApplyResult;

enum class MaintenanceKind { Validate, Compact, RebuildDerived };

struct MaintenanceResult {
    MaintenanceKind kind = MaintenanceKind::Validate;
    bool completed = false;
    std::string detail;
};

enum class PortfolioStoreErrorCode {
    InvalidInput,
    CursorConflict,
    IdempotencyConflict,
    UnsupportedCapability,
};

class PortfolioStoreError : public std::runtime_error {
public:
    PortfolioStoreError(PortfolioStoreErrorCode code, const std::string& message);
    PortfolioStoreErrorCode code() const noexcept { return code_; }

private:
    PortfolioStoreErrorCode code_;
};

} // namespace axon::portfolio
