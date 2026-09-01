#include "portfolio/application/portfolio_store.hpp"

#ifdef AXON_PORTFOLIO_STORE_FACTORY_HEADER
#include AXON_PORTFOLIO_STORE_FACTORY_HEADER
#else
#include "portfolio/application/reference_portfolio_store.hpp"
#endif

#include <gtest/gtest.h>
#include <algorithm>
#include <functional>
#include <limits>
#include <memory>

namespace {

using namespace axon::portfolio;

// Every adapter compiles this exact source with its factory macro. The default target exercises the
// deterministic reference adapter; provider integration targets supply a header and factory.
std::unique_ptr<PortfolioStore> make_conformance_store() {
#ifdef AXON_PORTFOLIO_STORE_FACTORY
    return AXON_PORTFOLIO_STORE_FACTORY();
#else
    return std::make_unique<ReferencePortfolioStore>();
#endif
}

const RepositoryStreamKey kStream{"7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb",
                                  "4b809f2e-5606-4f45-b050-e4dbb30cde53"};
const RepositoryStreamKey kOtherStream{"ea75c2b6-f4a8-4bd6-a86c-fe52763bb33b",
                                       "d2be0631-0d2c-40cf-a997-1bc7f4618822"};
const RepositoryStreamKey kReidentifiedStream{"11111111-2222-4333-8444-555555555555",
                                              kStream.index_stream_id};
const RepositoryStreamKey kTwiceReidentifiedStream{"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
                                                   kStream.index_stream_id};
constexpr std::size_t kInspectionLimit = 1000;

ProjectionEvent event(const RepositoryStreamKey& stream, std::uint64_t sequence, std::string id,
                      std::vector<ProjectionMutation> mutations) {
    return {stream,
            sequence,
            std::move(id),
            "epoch-0000000001",
            "manifest-0000001",
            std::move(mutations)};
}

ProjectionEvent event(std::uint64_t sequence, std::string id,
                      std::vector<ProjectionMutation> mutations) {
    return event(kStream, sequence, std::move(id), std::move(mutations));
}

bool contains(const StreamProjection& projection, const std::string& kind, const std::string& key) {
    return std::any_of(projection.entities.begin(), projection.entities.end(),
                       [&](const ProjectionMutation& entity) {
                           return entity.entity_kind == kind && entity.entity_key == key;
                       });
}

StreamProjection inspect(PortfolioStore& store, const RepositoryStreamKey& stream) {
    return store.inspect_repository_stream(stream, kInspectionLimit);
}

void expect_error(PortfolioStoreErrorCode code, const std::function<void()>& operation) {
    try {
        operation();
        FAIL() << "expected PortfolioStoreError";
    } catch (const PortfolioStoreError& error) {
        EXPECT_EQ(error.code(), code) << error.what();
    }
}

TEST(PortfolioStoreConformance, AdvertisesRoleSpecificCapabilitiesHealthAndVersions) {
    auto store = make_conformance_store();
    const auto caps = store->capabilities();
    EXPECT_EQ(caps.role, ProviderRole::PortfolioStore);
    EXPECT_TRUE(caps.supports(ProviderCapability::AtomicApply));
    EXPECT_TRUE(caps.supports(ProviderCapability::ReplaceRepositoryStream));
    EXPECT_TRUE(caps.supports(ProviderCapability::ReidentifyRepositoryStream));
    EXPECT_FALSE(caps.supports(ProviderCapability::CapabilityQueries));
    EXPECT_GE(caps.max_batch_size, 1u);
    EXPECT_EQ(caps.max_batch_size, caps.max_events_per_batch);
    EXPECT_EQ(caps.max_events_per_batch, 500u);
    EXPECT_EQ(caps.max_mutations_per_event, 10000u);
    EXPECT_EQ(caps.max_snapshot_entities, 10000u);
    EXPECT_EQ(store->health().status, ProviderHealthStatus::Healthy);
    EXPECT_EQ(store->schema_version(), "axon/portfolio-store/v1");
    EXPECT_EQ(store->protocol_version(), "axon/portfolio-provider/v1");
    EXPECT_TRUE(store->maintenance(MaintenanceKind::Validate).completed);
    if (caps.supports(ProviderCapability::CompactMaintenance))
        EXPECT_TRUE(store->maintenance(MaintenanceKind::Compact).completed);
    else
        expect_error(PortfolioStoreErrorCode::UnsupportedCapability,
                     [&] { (void)store->maintenance(MaintenanceKind::Compact); });
    if (caps.supports(ProviderCapability::RebuildDerivedMaintenance))
        EXPECT_TRUE(store->maintenance(MaintenanceKind::RebuildDerived).completed);
    else
        expect_error(PortfolioStoreErrorCode::UnsupportedCapability,
                     [&] { (void)store->maintenance(MaintenanceKind::RebuildDerived); });
}

TEST(PortfolioStoreConformance, AppliesContiguousBatchAndReplaysIdempotently) {
    auto store = make_conformance_store();
    const std::vector<ProjectionEvent> batch = {
        event(1, "event-0000000001",
              {{"file", "src/a.cpp", ProjectionOperation::Upsert, "digest-000000001"}}),
        event(2, "event-0000000002",
              {{"symbol", "A::run", ProjectionOperation::Upsert, "digest-000000002"}})};
    const auto applied = store->apply(kStream, 0, batch);
    EXPECT_EQ(applied.disposition, ApplyDisposition::Applied);
    EXPECT_EQ(applied.state.cursor, 2u);
    EXPECT_EQ(inspect(*store, kStream).entities.size(), 2u);
    const auto limited = store->inspect_repository_stream(kStream, 1);
    EXPECT_EQ(limited.entities.size(), 1u);
    EXPECT_TRUE(limited.truncated);

    const auto replayed = store->apply(kStream, 0, batch);
    EXPECT_EQ(replayed.disposition, ApplyDisposition::Duplicate);
    EXPECT_EQ(replayed.state.cursor, 2u);
    EXPECT_EQ(inspect(*store, kStream).entities.size(), 2u);
}

TEST(PortfolioStoreConformance, DeltaWithoutManifestPreservesLastVerifiedSnapshotManifest) {
    auto store = make_conformance_store();
    auto snapshot_event =
        event(1, "snapshot-event-001",
              {{"file", "src/a.cpp", ProjectionOperation::Upsert, "digest-000000001"}});
    snapshot_event.manifest = "snapshot-manifest-0001";
    auto delta = event(2, "delta-event-00001",
                       {{"file", "src/b.cpp", ProjectionOperation::Upsert, "digest-000000002"}});
    delta.epoch = "epoch-0000000002";
    delta.manifest = std::nullopt;

    const auto applied = store->apply(kStream, 0, {snapshot_event, delta});
    EXPECT_EQ(applied.state.cursor, 2u);
    EXPECT_EQ(applied.state.epoch, "epoch-0000000002");
    EXPECT_EQ(applied.state.manifest, "snapshot-manifest-0001");
    EXPECT_EQ(store->apply(kStream, 0, {snapshot_event, delta}).disposition,
              ApplyDisposition::Duplicate);

    auto verified = event(3, "verified-event-001", {});
    verified.epoch = "epoch-0000000003";
    verified.manifest = "snapshot-manifest-0002";
    const auto advanced = store->apply(kStream, 2, {verified});
    EXPECT_EQ(advanced.state.cursor, 3u);
    EXPECT_EQ(advanced.state.epoch, "epoch-0000000003");
    EXPECT_EQ(advanced.state.manifest, "snapshot-manifest-0002");

    auto initial_delta = event(kOtherStream, 1, "initial-delta-001", {});
    initial_delta.manifest = std::nullopt;
    const auto initial = store->apply(kOtherStream, 0, {initial_delta});
    EXPECT_EQ(initial.state.cursor, 1u);
    EXPECT_TRUE(initial.state.manifest.empty());
}

TEST(PortfolioStoreConformance, CursorFailureAndInvalidTailAreAtomic) {
    auto store = make_conformance_store();
    store->apply(kStream, 0,
                 {event(1, "event-0000000001",
                        {{"file", "src/a.cpp", ProjectionOperation::Upsert, "digest-000000001"}})});
    expect_error(PortfolioStoreErrorCode::CursorConflict, [&] {
        store->apply(
            kStream, 0,
            {event(1, "different-event-01",
                   {{"file", "src/b.cpp", ProjectionOperation::Upsert, "digest-000000002"}})});
    });
    const std::vector<ProjectionEvent> invalid_tail = {
        event(2, "event-0000000002",
              {{"file", "src/b.cpp", ProjectionOperation::Upsert, "digest-000000002"}}),
        event(4, "event-0000000004",
              {{"file", "src/c.cpp", ProjectionOperation::Upsert, "digest-000000004"}})};
    expect_error(PortfolioStoreErrorCode::CursorConflict,
                 [&] { store->apply(kStream, 1, invalid_tail); });
    const auto projection = inspect(*store, kStream);
    EXPECT_EQ(projection.state.cursor, 1u);
    ASSERT_EQ(projection.entities.size(), 1u);
    EXPECT_TRUE(contains(projection, "file", "src/a.cpp"));
}

TEST(PortfolioStoreConformance, EventIdConflictsAreGlobalAndAtomic) {
    auto store = make_conformance_store();
    store->apply(kStream, 0,
                 {event(1, "shared-event-0001",
                        {{"file", "src/a.cpp", ProjectionOperation::Upsert, "digest-000000001"}})});
    expect_error(PortfolioStoreErrorCode::IdempotencyConflict, [&] {
        store->apply(
            kStream, 1,
            {event(2, "shared-event-0001",
                   {{"file", "src/b.cpp", ProjectionOperation::Upsert, "digest-000000002"}})});
    });
    expect_error(PortfolioStoreErrorCode::IdempotencyConflict, [&] {
        store->apply(
            kOtherStream, 0,
            {event(kOtherStream, 1, "shared-event-0001",
                   {{"file", "src/other.cpp", ProjectionOperation::Upsert, "digest-000000003"}})});
    });
    EXPECT_EQ(store->stream_state(kStream).cursor, 1u);
    EXPECT_FALSE(store->stream_state(kOtherStream).exists);
    EXPECT_FALSE(contains(inspect(*store, kStream), "file", "src/b.cpp"));
}

TEST(PortfolioStoreConformance, PartialReplayIsDuplicateButMixedReplayConflicts) {
    auto store = make_conformance_store();
    const auto first =
        event(1, "event-0000000001",
              {{"file", "src/a.cpp", ProjectionOperation::Upsert, "digest-000000001"}});
    const auto second =
        event(2, "event-0000000002",
              {{"file", "src/b.cpp", ProjectionOperation::Upsert, "digest-000000002"}});
    store->apply(kStream, 0, {first, second});
    EXPECT_EQ(store->apply(kStream, 0, {first}).disposition, ApplyDisposition::Duplicate);
    expect_error(PortfolioStoreErrorCode::CursorConflict, [&] {
        store->apply(kStream, 0, {second, first});
    });
    expect_error(PortfolioStoreErrorCode::CursorConflict,
                 [&] { store->apply(kStream, 1, {first}); });
    expect_error(PortfolioStoreErrorCode::CursorConflict, [&] {
        store->apply(kStream, 1,
                     {second, event(3, "event-0000000003",
                                    {{"file", "src/c.cpp", ProjectionOperation::Upsert,
                                      "digest-000000003"}})});
    });
    const auto projection = inspect(*store, kStream);
    EXPECT_EQ(projection.state.cursor, 2u);
    EXPECT_EQ(projection.entities.size(), 2u);
    EXPECT_FALSE(contains(projection, "file", "src/c.cpp"));
}

TEST(PortfolioStoreConformance, DeleteAndReplaceArePartitionScopedAndIdempotent) {
    auto store = make_conformance_store();
    store->apply(
        kOtherStream, 0,
        {event(kOtherStream, 1, "other-event-0001",
               {{"file", "src/other.cpp", ProjectionOperation::Upsert, "digest-000000010"}})});
    store->apply(kStream, 0,
                 {event(1, "event-0000000001",
                        {{"file", "src/a.cpp", ProjectionOperation::Upsert, "digest-000000001"},
                         {"file", "src/b.cpp", ProjectionOperation::Upsert, "digest-000000002"}})});
    store->apply(kStream, 1,
                 {event(2, "event-0000000002",
                        {{"file", "src/a.cpp", ProjectionOperation::Delete, std::nullopt}})});
    auto projection = inspect(*store, kStream);
    EXPECT_FALSE(contains(projection, "file", "src/a.cpp"));
    EXPECT_TRUE(contains(projection, "file", "src/b.cpp"));

    const RepositorySnapshot snapshot = {
        kStream,
        2,
        "epoch-0000000002",
        "manifest-0000002",
        false,
        false,
        {{"file", "src/c.cpp", ProjectionOperation::Upsert, "digest-000000003"}}};
    EXPECT_EQ(store->replace_repository_stream(snapshot, 2).disposition, ApplyDisposition::Applied);
    projection = inspect(*store, kStream);
    ASSERT_EQ(projection.entities.size(), 1u);
    EXPECT_TRUE(contains(projection, "file", "src/c.cpp"));
    EXPECT_EQ(store->replace_repository_stream(snapshot, 2).disposition,
              ApplyDisposition::Duplicate);

    const auto other = inspect(*store, kOtherStream);
    EXPECT_EQ(other.state.cursor, 1u);
    ASSERT_EQ(other.entities.size(), 1u);
    EXPECT_TRUE(contains(other, "file", "src/other.cpp"));
}

TEST(PortfolioStoreConformance, EnforcesAdvertisedBatchMutationAndFieldBounds) {
    auto store = make_conformance_store();
    expect_error(PortfolioStoreErrorCode::InvalidInput, [&] { store->apply(kStream, 0, {}); });

    std::vector<ProjectionEvent> oversized_batch;
    oversized_batch.reserve(store->capabilities().max_events_per_batch + 1);
    for (std::size_t i = 0; i <= store->capabilities().max_events_per_batch; ++i)
        oversized_batch.push_back(event(i + 1, "bounded-event-" + std::to_string(100000 + i), {}));
    expect_error(PortfolioStoreErrorCode::InvalidInput,
                 [&] { store->apply(kStream, 0, oversized_batch); });

    std::vector<ProjectionMutation> oversized_mutations(
        store->capabilities().max_mutations_per_event + 1,
        {"file", "src/a.cpp", ProjectionOperation::Upsert, "digest-000000001"});
    expect_error(PortfolioStoreErrorCode::InvalidInput, [&] {
        store->apply(kStream, 0, {event(1, "bounded-event-0001", oversized_mutations)});
    });
    expect_error(PortfolioStoreErrorCode::InvalidInput,
                 [&] { store->apply(kStream, 0, {event(1, "short", {})}); });
    expect_error(PortfolioStoreErrorCode::InvalidInput, [&] {
        store->apply(
            kStream, 0,
            {event(1, "bounded-event-0001",
                   {{std::string(129, 'k'), "x", ProjectionOperation::Upsert, std::nullopt}})});
    });
    expect_error(PortfolioStoreErrorCode::InvalidInput, [&] {
        store->apply(
            kStream, 0,
            {event(1, "bounded-event-0001",
                   {{"file", std::string(4097, 'x'), ProjectionOperation::Upsert, std::nullopt}})});
    });
    expect_error(PortfolioStoreErrorCode::InvalidInput, [&] {
        store->apply(kStream, 0,
                     {event(1, "bounded-event-0001",
                            {{"file", "src/a.cpp", ProjectionOperation::Upsert, "short"}})});
    });
    expect_error(PortfolioStoreErrorCode::InvalidInput,
                 [&] { store->apply(kStream, 0, {event(1, std::string(129, 'e'), {})}); });
    auto invalid_manifest = event(1, "bounded-event-0001", {});
    invalid_manifest.manifest = "short";
    expect_error(PortfolioStoreErrorCode::InvalidInput,
                 [&] { store->apply(kStream, 0, {invalid_manifest}); });
    RepositoryStreamKey invalid = kStream;
    invalid.repository_id = "not-a-uuid";
    expect_error(PortfolioStoreErrorCode::InvalidInput,
                 [&] { (void)store->stream_state(invalid); });
    expect_error(PortfolioStoreErrorCode::InvalidInput,
                 [&] { (void)store->inspect_repository_stream(kStream, 0); });
    EXPECT_FALSE(store->stream_state(kStream).exists);
}

TEST(PortfolioStoreConformance, AcceptsJournalCardinalityAndBoundsSnapshotsSeparately) {
    auto store = make_conformance_store();
    const auto caps = store->capabilities();
    std::vector<ProjectionMutation> mutations;
    mutations.reserve(caps.max_mutations_per_event);
    for (std::size_t index = 0; index < caps.max_mutations_per_event; ++index)
        mutations.push_back({"file", "src/file-" + std::to_string(index),
                             ProjectionOperation::Upsert,
                             "digest-cardinality-" + std::to_string(100000 + index)});
    EXPECT_EQ(
        store->apply(kStream, 0, {event(1, "cardinality-event-0001", mutations)}).mutations_applied,
        caps.max_mutations_per_event);
    const auto projection = store->inspect_repository_stream(kStream, caps.max_snapshot_entities);
    EXPECT_EQ(projection.entities.size(), caps.max_mutations_per_event);
    EXPECT_FALSE(projection.truncated);

    RepositorySnapshot snapshot{
        kOtherStream, 1,     "snapshot-epoch-0001", "snapshot-manifest-0001",
        false,        false, projection.entities};
    EXPECT_EQ(store->replace_repository_stream(snapshot, 0).mutations_applied,
              caps.max_snapshot_entities);
    snapshot.entities.push_back(
        {"file", "overflow", ProjectionOperation::Upsert, "digest-cardinality-overflow"});
    expect_error(PortfolioStoreErrorCode::InvalidInput,
                 [&] { store->replace_repository_stream(snapshot, 1); });
}

TEST(PortfolioStoreConformance, ReidentifiesPhysicalStreamAtomicallyAndIdempotently) {
    auto store = make_conformance_store();
    const auto initial =
        event(1, "before-handoff-event-001",
              {{"file", "src/a.cpp", ProjectionOperation::Upsert, "digest-before-handoff"}});
    store->apply(kStream, 0, {initial});
    RepositoryReidentification handoff{kStream,
                                       kReidentifiedStream,
                                       2,
                                       "repository-handoff-event-01",
                                       "handoff-epoch-0001",
                                       std::nullopt,
                                       "old-binding-0000001",
                                       "new-binding-0000001",
                                       "ADR-0003-owner-approval",
                                       "owner-approved"};
    const auto applied = store->reidentify_repository_stream(handoff, 1);
    EXPECT_EQ(applied.disposition, ApplyDisposition::Applied);
    EXPECT_FALSE(store->stream_state(kStream).exists);
    EXPECT_EQ(store->stream_state(kReidentifiedStream).cursor, 2u);
    EXPECT_TRUE(contains(inspect(*store, kReidentifiedStream), "file", "src/a.cpp"));
    EXPECT_TRUE(contains(inspect(*store, kReidentifiedStream), "repository",
                         kReidentifiedStream.repository_id));
    EXPECT_FALSE(
        contains(inspect(*store, kReidentifiedStream), "repository", kStream.repository_id));
    EXPECT_EQ(store->reidentify_repository_stream(handoff, 1).disposition,
              ApplyDisposition::Duplicate);
    EXPECT_EQ(store->apply(kStream, 0, {initial}).state.cursor, 2u);

    const auto after =
        event(kReidentifiedStream, 3, "after-handoff-event-0001",
              {{"file", "src/b.cpp", ProjectionOperation::Upsert, "digest-after-handoff"}});
    EXPECT_EQ(store->apply(kReidentifiedStream, 2, {after}).state.cursor, 3u);
    const auto late_replay = store->reidentify_repository_stream(handoff, 1);
    EXPECT_EQ(late_replay.disposition, ApplyDisposition::Duplicate);
    EXPECT_EQ(late_replay.state.cursor, 3u);
    expect_error(PortfolioStoreErrorCode::IdempotencyConflict, [&] {
        store->apply(kReidentifiedStream, 3, {event(kReidentifiedStream, 4, handoff.event_id, {})});
    });
    RepositoryReidentification second_handoff{
        kReidentifiedStream,         kTwiceReidentifiedStream, 4,
        "second-handoff-event-0001", "handoff-epoch-0002",     "handoff-manifest-0002",
        "new-binding-0000001",       "next-binding-000001",    "ADR-0003-owner-approval",
        "repository-moved"};
    EXPECT_EQ(store->reidentify_repository_stream(second_handoff, 3).state.cursor, 4u);
    EXPECT_FALSE(store->stream_state(kReidentifiedStream).exists);
    EXPECT_TRUE(contains(inspect(*store, kTwiceReidentifiedStream), "file", "src/a.cpp"));
    EXPECT_EQ(store->reidentify_repository_stream(handoff, 1).state.cursor, 4u);

    auto changed = handoff;
    changed.current_stream.repository_id = "99999999-2222-4333-8444-555555555555";
    expect_error(PortfolioStoreErrorCode::IdempotencyConflict,
                 [&] { store->reidentify_repository_stream(changed, 1); });
}

TEST(PortfolioStoreConformance, ReidentificationCollisionAndInvalidShapeAreAtomic) {
    auto store = make_conformance_store();
    store->apply(kStream, 0, {event(1, "old-partition-event-0001", {})});
    expect_error(PortfolioStoreErrorCode::IdempotencyConflict, [&] {
        store->apply(kReidentifiedStream, 0,
                     {event(kReidentifiedStream, 1, "target-partition-event-01", {})});
    });
    RepositoryReidentification handoff{kStream,
                                       kReidentifiedStream,
                                       2,
                                       "collision-handoff-event-01",
                                       "handoff-epoch-0001",
                                       "handoff-manifest-0001",
                                       "old-binding-0000001",
                                       "new-binding-0000001",
                                       "ADR-0003-owner-approval",
                                       "owner-approved"};
    EXPECT_EQ(store->reidentify_repository_stream(handoff, 1).disposition,
              ApplyDisposition::Applied);
    EXPECT_FALSE(store->stream_state(kStream).exists);
    EXPECT_EQ(store->stream_state(kReidentifiedStream).cursor, 2u);

    auto invalid = handoff;
    invalid.current_stream.index_stream_id = kOtherStream.index_stream_id;
    expect_error(PortfolioStoreErrorCode::InvalidInput,
                 [&] { store->reidentify_repository_stream(invalid, 1); });
    invalid = handoff;
    invalid.reason = "unapproved";
    expect_error(PortfolioStoreErrorCode::InvalidInput,
                 [&] { store->reidentify_repository_stream(invalid, 1); });
    invalid = handoff;
    invalid.old_binding_id = "short";
    expect_error(PortfolioStoreErrorCode::InvalidInput,
                 [&] { store->reidentify_repository_stream(invalid, 1); });
    invalid = handoff;
    invalid.event_id = "different-handoff-event-01";
    invalid.sequence = 3;
    expect_error(PortfolioStoreErrorCode::CursorConflict,
                 [&] { store->reidentify_repository_stream(invalid, 1); });
    EXPECT_EQ(store->stream_state(kReidentifiedStream).cursor, 2u);

    auto removed_store = make_conformance_store();
    RepositorySnapshot removed{kStream, 1, "removed-epoch-0001", "removed-manifest-0001", false,
                               true,    {}};
    removed_store->replace_repository_stream(removed, 0);
    expect_error(PortfolioStoreErrorCode::InvalidInput,
                 [&] { removed_store->reidentify_repository_stream(handoff, 1); });
    EXPECT_TRUE(removed_store->stream_state(kStream).removed);
}

TEST(PortfolioStoreConformance, RejectsZeroSnapshotAndNeverWrapsExhaustedCursor) {
    auto store = make_conformance_store();
    RepositorySnapshot snapshot = {kStream, 0, "epoch-0000000001", "manifest-0000001", false,
                                   false,   {}};
    expect_error(PortfolioStoreErrorCode::InvalidInput,
                 [&] { store->replace_repository_stream(snapshot, 0); });
    EXPECT_FALSE(store->stream_state(kStream).exists);

    snapshot.cursor = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(store->replace_repository_stream(snapshot, 0).disposition, ApplyDisposition::Applied);
    expect_error(PortfolioStoreErrorCode::CursorConflict, [&] {
        store->apply(kStream, std::numeric_limits<std::uint64_t>::max(),
                     {event(0, "wrapped-event-001", {})});
    });
    EXPECT_EQ(store->stream_state(kStream).cursor, std::numeric_limits<std::uint64_t>::max());
    EXPECT_TRUE(store->maintenance(MaintenanceKind::Validate).completed);
}

} // namespace
