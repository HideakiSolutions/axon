#include "reference_portfolio_store.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <unordered_set>

namespace axon::portfolio {
namespace {

constexpr std::size_t kReferenceMaxEventsPerBatch = 500;
constexpr std::size_t kReferenceMaxMutationsPerEvent = 10000;
constexpr std::size_t kReferenceMaxSnapshotEntities = 10000;
const std::unordered_set<std::string> kReidentificationReasons = {
    "contract-adopted", "collision-repaired", "repository-moved", "owner-approved"};

bool canonical_uuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    return true;
}

[[noreturn]] void fail(PortfolioStoreErrorCode code, const std::string& message) {
    throw PortfolioStoreError(code, message);
}

void validate_mutation(const ProjectionMutation& mutation) {
    if (mutation.entity_kind.empty() || mutation.entity_kind.size() > 128 ||
        mutation.entity_key.empty() || mutation.entity_key.size() > 4096)
        fail(PortfolioStoreErrorCode::InvalidInput, "projection mutation identity is invalid");
    if (mutation.digest && (mutation.digest->size() < 16 || mutation.digest->size() > 128))
        fail(PortfolioStoreErrorCode::InvalidInput, "projection mutation digest is invalid");
}

} // namespace

ProviderCapabilities ReferencePortfolioStore::capabilities() const {
    return {ProviderRole::PortfolioStore,
            {ProviderCapability::AtomicApply, ProviderCapability::ReplaceRepositoryStream,
             ProviderCapability::ReidentifyRepositoryStream,
             ProviderCapability::ValidateMaintenance},
            kReferenceMaxEventsPerBatch, kReferenceMaxEventsPerBatch,
            kReferenceMaxMutationsPerEvent, kReferenceMaxSnapshotEntities};
}

ProviderHealth ReferencePortfolioStore::health() const {
    return {ProviderHealthStatus::Healthy, "reference adapter ready"};
}

std::string ReferencePortfolioStore::schema_version() const {
    return "axon/portfolio-store/v1";
}

std::string ReferencePortfolioStore::protocol_version() const {
    return "axon/portfolio-provider/v1";
}

void ReferencePortfolioStore::validate_stream(const RepositoryStreamKey& stream) {
    if (!canonical_uuid(stream.repository_id) || !canonical_uuid(stream.index_stream_id))
        fail(PortfolioStoreErrorCode::InvalidInput, "repository stream identity is invalid");
}

void ReferencePortfolioStore::validate_event(const ProjectionEvent& event,
                                             const RepositoryStreamKey& stream,
                                             std::uint64_t expected_sequence,
                                             std::size_t max_batch_size) {
    if (event.stream != stream)
        fail(PortfolioStoreErrorCode::InvalidInput, "event stream identity does not match batch");
    if (event.sequence != expected_sequence)
        fail(PortfolioStoreErrorCode::CursorConflict, "event sequence is not contiguous");
    if (event.event_id.size() < 16 || event.event_id.size() > 128 || event.epoch.size() < 16 ||
        event.epoch.size() > 128 ||
        (event.manifest && (event.manifest->size() < 16 || event.manifest->size() > 128)) ||
        event.mutations.size() > max_batch_size)
        fail(PortfolioStoreErrorCode::InvalidInput, "projection event bounds are invalid");
    for (const auto& mutation : event.mutations) validate_mutation(mutation);
}

ApplyResult ReferencePortfolioStore::apply(const RepositoryStreamKey& stream,
                                           std::uint64_t expected_cursor,
                                           const std::vector<ProjectionEvent>& events) {
    validate_stream(stream);
    if (events.empty() || events.size() > kReferenceMaxEventsPerBatch)
        fail(PortfolioStoreErrorCode::InvalidInput, "event batch size is invalid");

    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = streams_.find(stream);
    const StreamData current = existing == streams_.end() ? StreamData{} : existing->second;

    std::uint64_t validated_sequence = expected_cursor;
    for (const auto& event : events) {
        if (validated_sequence == std::numeric_limits<std::uint64_t>::max())
            fail(PortfolioStoreErrorCode::CursorConflict,
                 "portfolio stream sequence is exhausted");
        validate_event(event, stream, ++validated_sequence, kReferenceMaxMutationsPerEvent);
    }

    bool exact_replay = true;
    for (const auto& event : events) {
        const auto receipt = receipts_.find(event.event_id);
        if (receipt == receipts_.end() || receipt->second.stream != stream ||
            receipt->second.sequence != event.sequence || receipt->second.event != event) {
            exact_replay = false;
            break;
        }
    }
    if (exact_replay) {
        if (existing != streams_.end())
            return {ApplyDisposition::Duplicate, current.state, 0};
        for (const auto& [key, data] : streams_)
            if (key.index_stream_id == stream.index_stream_id)
                return {ApplyDisposition::Duplicate, data.state, 0};
        fail(PortfolioStoreErrorCode::IdempotencyConflict,
             "event receipt has no matching physical stream partition");
    }

    if (existing == streams_.end()) {
        for (const auto& [key, data] : streams_) {
            (void)data;
            if (key.index_stream_id == stream.index_stream_id)
                fail(PortfolioStoreErrorCode::IdempotencyConflict,
                     "physical index stream is bound to another repository identity");
        }
    }

    if (current.state.cursor != expected_cursor)
        fail(PortfolioStoreErrorCode::CursorConflict, "portfolio stream cursor conflict");

    StreamData next = current;
    auto next_receipts = receipts_;
    std::size_t mutations_applied = 0;
    for (const auto& event : events) {
        const auto receipt = next_receipts.find(event.event_id);
        if (receipt != next_receipts.end() || reidentifications_.count(event.event_id) != 0)
            fail(PortfolioStoreErrorCode::IdempotencyConflict,
                 "event_id is already bound to another event");
        if (next.events.count(event.sequence) != 0)
            fail(PortfolioStoreErrorCode::IdempotencyConflict,
                 "sequence is already bound to another event");
        for (const auto& mutation : event.mutations) {
            const EntityKey key{mutation.entity_kind, mutation.entity_key};
            if (mutation.operation == ProjectionOperation::Delete)
                next.entities.erase(key);
            else
                next.entities[key] = mutation;
            ++mutations_applied;
        }
        next.events[event.sequence] = event;
        next_receipts.emplace(event.event_id, EventReceipt{stream, event.sequence, event});
        next.state = {true, event.sequence, event.epoch,
                      event.manifest.value_or(next.state.manifest), false, false};
    }
    streams_[stream] = std::move(next);
    receipts_ = std::move(next_receipts);
    return {ApplyDisposition::Applied, streams_.at(stream).state, mutations_applied};
}

std::map<ReferencePortfolioStore::EntityKey, ProjectionMutation>
ReferencePortfolioStore::snapshot_entities(const RepositorySnapshot& snapshot,
                                           std::size_t max_batch_size) {
    if (snapshot.entities.size() > max_batch_size)
        fail(PortfolioStoreErrorCode::InvalidInput, "snapshot entity count exceeds provider bound");
    std::map<EntityKey, ProjectionMutation> entities;
    for (const auto& entity : snapshot.entities) {
        validate_mutation(entity);
        if (entity.operation != ProjectionOperation::Upsert)
            fail(PortfolioStoreErrorCode::InvalidInput,
                 "repository snapshot may contain only present entities");
        if (!entities.emplace(EntityKey{entity.entity_kind, entity.entity_key}, entity).second)
            fail(PortfolioStoreErrorCode::InvalidInput, "repository snapshot has duplicate entity");
    }
    return entities;
}

ReplaceResult
ReferencePortfolioStore::replace_repository_stream(const RepositorySnapshot& snapshot,
                                                   std::uint64_t expected_cursor) {
    validate_stream(snapshot.stream);
    if (snapshot.cursor == 0 || snapshot.epoch.size() < 16 || snapshot.epoch.size() > 128 ||
        snapshot.manifest.size() < 16 || snapshot.manifest.size() > 128 ||
        snapshot.cursor < expected_cursor)
        fail(PortfolioStoreErrorCode::InvalidInput, "repository snapshot bounds are invalid");
    auto entities = snapshot_entities(snapshot, kReferenceMaxSnapshotEntities);

    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = streams_.find(snapshot.stream);
    const StreamData current = existing == streams_.end() ? StreamData{} : existing->second;
    const CursorEpochManifest replacement_state = {true, snapshot.cursor, snapshot.epoch,
                                                    snapshot.manifest, snapshot.stale,
                                                    snapshot.removed};
    if (current.state == replacement_state && current.entities == entities)
        return {ApplyDisposition::Duplicate, current.state, 0};
    if (existing == streams_.end()) {
        for (const auto& [key, data] : streams_) {
            (void)data;
            if (key.index_stream_id == snapshot.stream.index_stream_id)
                fail(PortfolioStoreErrorCode::IdempotencyConflict,
                     "physical index stream is bound to another repository identity");
        }
    }
    if (current.state.cursor != expected_cursor)
        fail(PortfolioStoreErrorCode::CursorConflict, "portfolio stream cursor conflict");

    StreamData replacement;
    replacement.state = replacement_state;
    replacement.entities = std::move(entities);
    streams_[snapshot.stream] = std::move(replacement);
    return {ApplyDisposition::Applied, streams_.at(snapshot.stream).state,
            streams_.at(snapshot.stream).entities.size()};
}

ApplyResult ReferencePortfolioStore::reidentify_repository_stream(
    const RepositoryReidentification& reidentification, std::uint64_t expected_cursor) {
    validate_stream(reidentification.previous_stream);
    validate_stream(reidentification.current_stream);
    if (reidentification.previous_stream.repository_id ==
            reidentification.current_stream.repository_id ||
        reidentification.previous_stream.index_stream_id !=
            reidentification.current_stream.index_stream_id ||
        reidentification.sequence == 0 ||
        reidentification.event_id.size() < 16 || reidentification.event_id.size() > 128 ||
        reidentification.epoch.size() < 16 || reidentification.epoch.size() > 128 ||
        (reidentification.manifest && (reidentification.manifest->size() < 16 ||
                                       reidentification.manifest->size() > 128)) ||
        reidentification.old_binding_id.size() < 16 ||
        reidentification.old_binding_id.size() > 128 ||
        reidentification.new_binding_id.size() < 16 ||
        reidentification.new_binding_id.size() > 128 ||
        reidentification.approval_reference.empty() ||
        reidentification.approval_reference.size() > 512 ||
        kReidentificationReasons.count(reidentification.reason) == 0)
        fail(PortfolioStoreErrorCode::InvalidInput,
             "repository reidentification bounds are invalid");
    if (expected_cursor == std::numeric_limits<std::uint64_t>::max() ||
        reidentification.sequence != expected_cursor + 1)
        fail(PortfolioStoreErrorCode::CursorConflict,
             "repository reidentification sequence is not contiguous");

    std::lock_guard<std::mutex> lock(mutex_);
    const auto replay = reidentifications_.find(reidentification.event_id);
    if (replay != reidentifications_.end()) {
        if (replay->second != reidentification)
            fail(PortfolioStoreErrorCode::IdempotencyConflict,
                 "reidentification event_id is bound to another handoff");
        auto current = streams_.find(reidentification.current_stream);
        if (current == streams_.end()) {
            current = std::find_if(streams_.begin(), streams_.end(), [&](const auto& entry) {
                return entry.first.index_stream_id ==
                       reidentification.current_stream.index_stream_id;
            });
        }
        if (current == streams_.end() || current->second.state.cursor < reidentification.sequence)
            fail(PortfolioStoreErrorCode::IdempotencyConflict,
                 "reidentification receipt has no matching current partition");
        return {ApplyDisposition::Duplicate, current->second.state, 0};
    }
    if (receipts_.count(reidentification.event_id) != 0)
        fail(PortfolioStoreErrorCode::IdempotencyConflict,
             "reidentification event_id is already bound to a projection event");
    const auto previous = streams_.find(reidentification.previous_stream);
    if (previous == streams_.end() || previous->second.state.cursor != expected_cursor)
        fail(PortfolioStoreErrorCode::CursorConflict,
             "reidentification previous cursor conflict");
    if (previous->second.state.removed)
        fail(PortfolioStoreErrorCode::InvalidInput,
             "removed repository stream cannot be reidentified");
    if (streams_.count(reidentification.current_stream) != 0)
        fail(PortfolioStoreErrorCode::IdempotencyConflict,
             "reidentification target partition already exists");

    StreamData transferred = previous->second;
    transferred.state.cursor = reidentification.sequence;
    transferred.state.epoch = reidentification.epoch;
    transferred.state.manifest =
        reidentification.manifest.value_or(transferred.state.manifest);
    transferred.state.stale = false;
    transferred.state.removed = false;
    transferred.entities.erase(
        EntityKey{"repository", reidentification.previous_stream.repository_id});
    transferred.entities[EntityKey{"repository", reidentification.current_stream.repository_id}] =
        {"repository", reidentification.current_stream.repository_id,
         ProjectionOperation::Upsert, std::nullopt};
    streams_.erase(previous);
    streams_.emplace(reidentification.current_stream, std::move(transferred));
    reidentifications_.emplace(reidentification.event_id, reidentification);
    return {ApplyDisposition::Applied,
            streams_.at(reidentification.current_stream).state, 2};
}

CursorEpochManifest
ReferencePortfolioStore::stream_state(const RepositoryStreamKey& stream) const {
    validate_stream(stream);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = streams_.find(stream);
    return found == streams_.end() ? CursorEpochManifest{} : found->second.state;
}

StreamProjection
ReferencePortfolioStore::inspect_repository_stream(const RepositoryStreamKey& stream,
                                                   std::size_t max_entities) const {
    validate_stream(stream);
    if (max_entities == 0 || max_entities > 10000)
        fail(PortfolioStoreErrorCode::InvalidInput, "stream inspection limit is invalid");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = streams_.find(stream);
    if (found == streams_.end()) return {};
    StreamProjection projection;
    projection.state = found->second.state;
    projection.entities.reserve(std::min(max_entities, found->second.entities.size()));
    for (const auto& [key, entity] : found->second.entities) {
        if (projection.entities.size() == max_entities) {
            projection.truncated = true;
            break;
        }
        (void)key;
        projection.entities.push_back(entity);
    }
    return projection;
}

MaintenanceResult ReferencePortfolioStore::maintenance(MaintenanceKind kind) {
    if (kind != MaintenanceKind::Validate)
        fail(PortfolioStoreErrorCode::UnsupportedCapability,
             "reference provider does not support requested maintenance operation");
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [stream, data] : streams_) {
        if (!data.state.exists || data.state.cursor == 0)
            fail(PortfolioStoreErrorCode::IdempotencyConflict,
                 "reference provider contains an invalid stream state");
        (void)stream;
    }
    return {kind, true, "reference state is internally consistent"};
}

} // namespace axon::portfolio
