#include "portfolio/infrastructure/duckdb/duckdb_portfolio_store.hpp"
#include "portfolio/infrastructure/duckdb/duckdb_repository_projector.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

using namespace axon::portfolio;
namespace fs = std::filesystem;

const RepositoryStreamKey kPersistentStream{"7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb",
                                            "4b809f2e-5606-4f45-b050-e4dbb30cde53"};

fs::path temporary_database(const char* suffix) {
    return fs::temp_directory_path() /
           (std::string("axon-portfolio-") + suffix + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".duckdb");
}

fs::path temporary_repository(const char* suffix) {
    auto path = fs::temp_directory_path() /
                (std::string("axon-source-") + suffix + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path / ".axon");
    return path;
}

std::string file_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool contains(const StreamProjection& projection, const std::string& kind,
              const std::string& key) {
    return std::any_of(projection.entities.begin(), projection.entities.end(),
                       [&](const ProjectionMutation& entity) {
                           return entity.entity_kind == kind && entity.entity_key == key;
                       });
}

RepositoryStreamKey create_source(const fs::path& root, const std::string& repository_id,
                                  const std::string& stream_id) {
    const auto path = root / ".axon/index.duckdb";
    duckdb::DuckDB database(path.string());
    duckdb::Connection connection(database);
    auto require = [](const auto& result) {
        if (!result || result->HasError())
            throw std::runtime_error(result ? result->GetError() : "missing DuckDB result");
    };
    require(connection.Query(
        "CREATE TABLE index_metadata(singleton BOOLEAN,repository_id VARCHAR,index_stream_id "
        "VARCHAR,current_epoch VARCHAR,current_manifest VARCHAR,removed BOOLEAN,schema_version VARCHAR)"));
    require(connection.Query(
        "CREATE TABLE index_events(repository_id VARCHAR,index_stream_id VARCHAR,sequence UBIGINT,"
        "event_id VARCHAR,index_epoch VARCHAR,manifest_hash VARCHAR,payload_json VARCHAR,"
        "schema_version VARCHAR,event_type VARCHAR)"));
    auto metadata = connection.Prepare(
        "INSERT INTO index_metadata VALUES(true,$1,$2,$3,$4,false,'axon/index-metadata/v1')");
    require(metadata->Execute(repository_id, stream_id, "source-epoch-0003",
                              "source-manifest-current"));
    auto event = connection.Prepare(
        "INSERT INTO index_events VALUES($1,$2,$3,$4,$5,$6,$7,'axon/index-event/v1',$8)");
    require(event->Execute(repository_id, stream_id, 1, "source-event-0001-" + repository_id,
                           "source-epoch-0001", "source-manifest-snapshot",
                           R"({"affected":[{"kind":"file","key":"a.cpp","operation":"upsert","digest":"source-digest-a-001"},{"kind":"file","key":"b.cpp","operation":"upsert","digest":"source-digest-b-001"}]})",
                           "IndexSnapshotCompleted"));
    require(event->Execute(repository_id, stream_id, 2, "source-event-0002-" + repository_id,
                           "source-epoch-0002", duckdb::Value(),
                           R"({"affected":[{"kind":"file","key":"a.cpp","operation":"upsert","digest":"source-digest-a-002"}]})",
                           "IndexFilesUpdated"));
    require(event->Execute(repository_id, stream_id, 3, "source-event-0003-" + repository_id,
                           "source-epoch-0003", duckdb::Value(),
                           R"({"affected":[{"kind":"file","key":"b.cpp","operation":"delete"}]})",
                           "IndexFilesDeleted"));
    return {repository_id, stream_id};
}

RepositoryStreamKey reidentify_source(const fs::path& root, const RepositoryStreamKey& previous,
                                      const std::string& current_repository_id,
                                      bool include_extra_mutation = false,
                                      bool include_identity_digest = false) {
    duckdb::DuckDB database((root / ".axon/index.duckdb").string());
    duckdb::Connection connection(database);
    auto insert = connection.Prepare(
        "INSERT INTO index_events VALUES($1,$2,4,$3,$4,NULL,$5,'axon/index-event/v1',"
        "'RepositoryReidentified')");
    const std::string payload_json =
        "{\"affected\":[{\"kind\":\"repository\",\"key\":\"" + previous.repository_id +
        "\",\"operation\":\"delete\"},{\"kind\":\"repository\",\"key\":\"" +
        current_repository_id +
        "\",\"operation\":\"upsert\"" +
        (include_identity_digest ? ",\"digest\":\"forbidden-identity-digest\"" : "") + "}" +
        (include_extra_mutation
             ? ",{\"kind\":\"file\",\"key\":\"hidden.cpp\",\"operation\":\"upsert\","
               "\"digest\":\"hidden-file-digest\"}"
             : "") +
        "],\"identity_change\":{"
        "\"old_repository_id\":\"" + previous.repository_id +
        "\",\"new_repository_id\":\"" + current_repository_id +
        "\",\"handoff_sequence\":4,\"old_binding_id\":\"old-binding-0000001\","
        "\"new_binding_id\":\"new-binding-0000001\","
        "\"approval_reference\":\"ADR-0003-owner-approval\","
        "\"reason\":\"owner-approved\"}}";
    auto result = insert->Execute(previous.repository_id, previous.index_stream_id,
                                  "source-reidentity-event-0001", "source-epoch-0004",
                                  payload_json.c_str());
    if (!result || result->HasError())
        throw std::runtime_error(result ? result->GetError() : "missing reidentity insert result");
    auto metadata = connection.Prepare(
        "UPDATE index_metadata SET repository_id=$1,current_epoch='',removed=false WHERE singleton=true");
    result = metadata->Execute(current_repository_id);
    if (!result || result->HasError())
        throw std::runtime_error(result ? result->GetError() : "missing identity update result");
    return {current_repository_id, previous.index_stream_id};
}

enum class InvalidHandoff {
    Reason,
    SameIdentity,
    PreviousUuid,
    CurrentUuid,
    StreamUuid,
    OldBindingShort,
    NewBindingLong,
    ApprovalEmpty,
    ApprovalLong,
    EventIdShort,
    EpochShort,
    ManifestShort
};

void corrupt_handoff(const fs::path& root, const RepositoryStreamKey& previous,
                     const RepositoryStreamKey& current, InvalidHandoff defect) {
    duckdb::DuckDB database((root / ".axon/index.duckdb").string());
    duckdb::Connection connection(database);
    auto selected = connection.Query("SELECT payload_json FROM index_events WHERE sequence=4");
    ASSERT_FALSE(selected->HasError());
    auto payload = nlohmann::json::parse(selected->GetValue(0, 0).ToString());
    std::string metadata_repository_id = current.repository_id;
    if (defect == InvalidHandoff::Reason) payload["identity_change"]["reason"] = "unapproved";
    if (defect == InvalidHandoff::SameIdentity) {
        payload["identity_change"]["new_repository_id"] = previous.repository_id;
        payload["affected"][1]["key"] = previous.repository_id;
        metadata_repository_id = previous.repository_id;
    }
    if (defect == InvalidHandoff::PreviousUuid) {
        payload["identity_change"]["old_repository_id"] = "invalid-previous-uuid";
        payload["affected"][0]["key"] = "invalid-previous-uuid";
        ASSERT_FALSE(connection.Query(
            "UPDATE index_events SET repository_id='invalid-previous-uuid'")->HasError());
    }
    if (defect == InvalidHandoff::CurrentUuid) {
        payload["identity_change"]["new_repository_id"] = "invalid-current-uuid";
        payload["affected"][1]["key"] = "invalid-current-uuid";
        metadata_repository_id = "invalid-current-uuid";
    }
    if (defect == InvalidHandoff::OldBindingShort)
        payload["identity_change"]["old_binding_id"] = "short";
    if (defect == InvalidHandoff::NewBindingLong)
        payload["identity_change"]["new_binding_id"] = std::string(129, 'b');
    if (defect == InvalidHandoff::ApprovalEmpty)
        payload["identity_change"]["approval_reference"] = "";
    if (defect == InvalidHandoff::ApprovalLong)
        payload["identity_change"]["approval_reference"] = std::string(513, 'a');

    auto update_payload = connection.Prepare(
        "UPDATE index_events SET payload_json=$1 WHERE sequence=4");
    ASSERT_FALSE(update_payload->Execute(payload.dump())->HasError());
    if (defect == InvalidHandoff::EventIdShort)
        ASSERT_FALSE(connection.Query(
            "UPDATE index_events SET event_id='short' WHERE sequence=4")->HasError());
    if (defect == InvalidHandoff::EpochShort)
        ASSERT_FALSE(connection.Query(
            "UPDATE index_events SET index_epoch='short' WHERE sequence=4")->HasError());
    if (defect == InvalidHandoff::ManifestShort)
        ASSERT_FALSE(connection.Query(
            "UPDATE index_events SET manifest_hash='short' WHERE sequence=4")->HasError());
    if (defect == InvalidHandoff::StreamUuid) {
        ASSERT_FALSE(connection.Query(
            "UPDATE index_events SET index_stream_id='invalid-stream-uuid'")->HasError());
        ASSERT_FALSE(connection.Query(
            "UPDATE index_metadata SET index_stream_id='invalid-stream-uuid'")->HasError());
    }
    auto metadata = connection.Prepare(
        "UPDATE index_metadata SET repository_id=$1 WHERE singleton=true");
    ASSERT_FALSE(metadata->Execute(metadata_repository_id)->HasError());
}

TEST(DuckdbPortfolioIntegration, PersistsCursorEntitiesAndManifestAcrossReopen) {
    const auto path = temporary_database("persist");
    {
        DuckdbPortfolioStore store(path);
        ProjectionEvent event = {kPersistentStream, 1, "persistent-event-1",
                                 "persistent-epoch-1", "persistent-manifest-1",
                                 {{"file", "src/main.cpp", ProjectionOperation::Upsert,
                                   "persistent-digest-1"}}};
        EXPECT_EQ(store.apply(kPersistentStream, 0, {event}).disposition,
                  ApplyDisposition::Applied);
    }
    {
        DuckdbPortfolioStore reopened(path);
        EXPECT_EQ(reopened.stream_state(kPersistentStream).cursor, 1u);
        const auto projection = reopened.inspect_repository_stream(kPersistentStream, 10);
        ASSERT_EQ(projection.entities.size(), 1u);
        EXPECT_EQ(projection.entities[0].entity_key, "src/main.cpp");
        EXPECT_EQ(projection.state.manifest, "persistent-manifest-1");
    }
    {
        duckdb::DuckDB database(path.string());
        duckdb::Connection connection(database);
        auto result = connection.Query("SELECT length(fingerprint) FROM portfolio_events");
        ASSERT_FALSE(result->HasError());
        EXPECT_EQ(result->GetValue<std::int64_t>(0, 0), 64);
    }
    std::error_code error;
    fs::remove(path, error);
}

TEST(DuckdbPortfolioIntegration, AdditiveMigrationPreservesUnrelatedOlderRows) {
    const auto path = temporary_database("upgrade");
    {
        duckdb::DuckDB old(path.string());
        duckdb::Connection connection(old);
        ASSERT_FALSE(connection.Query("CREATE TABLE legacy_marker(value VARCHAR)")->HasError());
        ASSERT_FALSE(connection.Query("INSERT INTO legacy_marker VALUES ('kept')")->HasError());
    }
    {
        DuckdbPortfolioStore upgraded(path);
        EXPECT_EQ(upgraded.schema_version(), "axon/portfolio-store/v1");
    }
    {
        duckdb::DuckDB read(path.string());
        duckdb::Connection connection(read);
        auto result = connection.Query("SELECT value FROM legacy_marker");
        ASSERT_FALSE(result->HasError()) << result->GetError();
        EXPECT_EQ(result->GetValue(0, 0).ToString(), "kept");
    }
    std::error_code error;
    fs::remove(path, error);
}

TEST(DuckdbPortfolioIntegration, MigrationChecksumMismatchFailsBeforeSchemaExpansion) {
    const auto path = temporary_database("bad-migration");
    {
        duckdb::DuckDB database(path.string());
        duckdb::Connection connection(database);
        ASSERT_FALSE(connection.Query(
            "CREATE TABLE portfolio_schema_migrations(version INTEGER PRIMARY KEY,"
            "checksum VARCHAR NOT NULL)")->HasError());
        ASSERT_FALSE(connection.Query(
            "INSERT INTO portfolio_schema_migrations VALUES(1,'unexpected-checksum')")
                         ->HasError());
    }
    EXPECT_THROW((void)DuckdbPortfolioStore(path), PortfolioStoreError);
    {
        duckdb::DuckDB database(path.string());
        duckdb::Connection connection(database);
        auto result = connection.Query(
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_name='portfolio_streams'");
        ASSERT_FALSE(result->HasError());
        EXPECT_EQ(result->GetValue<std::int64_t>(0, 0), 0);
    }
    std::error_code error;
    fs::remove(path, error);
}

TEST(DuckdbPortfolioIntegration, SnapshotModelsStaleRemovalAndReturnWithoutOtherStreamLoss) {
    DuckdbPortfolioStore store;
    RepositoryStreamKey other = kPersistentStream;
    other.index_stream_id = "d2be0631-0d2c-40cf-a997-1bc7f4618822";
    RepositorySnapshot active = {kPersistentStream, 1, "persistent-epoch-1",
                                 "persistent-manifest-1", false, false,
                                 {{"file", "a", ProjectionOperation::Upsert,
                                   "persistent-digest-1"}}};
    RepositorySnapshot other_active = {other, 1, "other-stream-epoch",
                                       "other-stream-manifest", false, false,
                                       {{"file", "other", ProjectionOperation::Upsert,
                                         "other-stream-digest"}}};
    store.replace_repository_stream(active, 0);
    store.replace_repository_stream(other_active, 0);
    auto stale = active; stale.stale = true; stale.cursor = 2; stale.epoch = "persistent-epoch-2";
    EXPECT_TRUE(store.replace_repository_stream(stale, 1).state.stale);
    auto removed = stale; removed.stale = false; removed.removed = true; removed.cursor = 3;
    removed.epoch = "persistent-epoch-3";
    EXPECT_TRUE(store.replace_repository_stream(removed, 2).state.removed);
    auto returned = active; returned.cursor = 4; returned.epoch = "persistent-epoch-4";
    EXPECT_FALSE(store.replace_repository_stream(returned, 3).state.removed);
    EXPECT_EQ(store.stream_state(other).cursor, 1u);
    EXPECT_EQ(store.inspect_repository_stream(other, 10).entities.size(), 1u);
}

TEST(DuckdbPortfolioIntegration, ProjectsThreeReadOnlyRepositoriesIncrementallyAndRebuilds) {
    const std::vector<std::pair<std::string, std::string>> identities = {
        {"10000000-0000-4000-8000-000000000001", "20000000-0000-4000-8000-000000000001"},
        {"10000000-0000-4000-8000-000000000002", "20000000-0000-4000-8000-000000000002"},
        {"10000000-0000-4000-8000-000000000003", "20000000-0000-4000-8000-000000000003"}};
    std::vector<fs::path> roots;
    DuckdbPortfolioStore incremental;
    DuckdbRepositoryProjector projector(incremental);
    for (std::size_t index = 0; index < identities.size(); ++index) {
        auto root = temporary_repository(("repo" + std::to_string(index)).c_str());
        roots.push_back(root);
        const auto stream = create_source(root, identities[index].first, identities[index].second);
        const auto source_path = root / ".axon/index.duckdb";
        const auto before = file_bytes(source_path);
        const auto applied = projector.sync(root, source_path, 1);
        EXPECT_EQ(applied.events_applied, 3u);
        EXPECT_EQ(applied.cursor_after, 3u);
        EXPECT_EQ(file_bytes(source_path), before) << "projector modified authoritative source";
        const auto projected = incremental.inspect_repository_stream(stream, 10);
        ASSERT_EQ(projected.entities.size(), 1u);
        EXPECT_EQ(projected.entities[0].entity_key, "a.cpp");
        EXPECT_EQ(projected.entities[0].digest, "source-digest-a-002");
        EXPECT_EQ(projected.state.manifest, "source-manifest-snapshot");
        EXPECT_EQ(projector.sync(root, source_path, 2).events_applied, 0u);

        EXPECT_TRUE(projector.mark_stale(stream));
        EXPECT_TRUE(incremental.stream_state(stream).stale);
        EXPECT_EQ(projector.sync(root, source_path, 2).events_applied, 0u);
        EXPECT_FALSE(incremental.stream_state(stream).stale);

        DuckdbPortfolioStore rebuilt;
        DuckdbRepositoryProjector rebuilder(rebuilt);
        EXPECT_TRUE(rebuilder.rebuild(root, source_path).rebuilt);
        EXPECT_EQ(rebuilt.inspect_repository_stream(stream, 10).entities, projected.entities);
        EXPECT_EQ(rebuilt.stream_state(stream).cursor, projected.state.cursor);
        EXPECT_EQ(rebuilt.stream_state(stream).epoch, projected.state.epoch);
        EXPECT_EQ(rebuilt.stream_state(stream).manifest, "source-manifest-snapshot");
    }
    for (const auto& root : roots) {
        std::error_code error;
        fs::remove_all(root, error);
    }
}

TEST(DuckdbPortfolioIntegration, RejectsSourceOutsideRegisteredRootAndSymlink) {
    const auto root = temporary_repository("boundary");
    const auto outside = temporary_repository("outside");
    create_source(root, "31000000-0000-4000-8000-000000000001",
                  "41000000-0000-4000-8000-000000000001");
    create_source(outside, "30000000-0000-4000-8000-000000000001",
                  "40000000-0000-4000-8000-000000000001");
    DuckdbPortfolioStore store;
    DuckdbRepositoryProjector projector(store);
    EXPECT_THROW(projector.sync(root, outside / ".axon/index.duckdb"), std::invalid_argument);
#ifndef _WIN32
    std::error_code error;
    fs::create_symlink(outside / ".axon/index.duckdb", root / ".axon/linked.duckdb", error);
    ASSERT_FALSE(error);
    EXPECT_THROW(projector.sync(root, root / ".axon/linked.duckdb"), std::invalid_argument);
    fs::remove(root / ".axon/linked.duckdb", error);
    fs::create_symlink(root / ".axon/index.duckdb", root / ".axon/linked-inside.duckdb", error);
    ASSERT_FALSE(error);
    EXPECT_THROW(projector.sync(root, root / ".axon/linked-inside.duckdb"),
                 std::invalid_argument);
#endif
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    fs::remove_all(outside, cleanup_error);
}

TEST(DuckdbPortfolioIntegration, PersistentApplyRollsBackPartialConflictAndReplaysAfterCommit) {
    const auto path = temporary_database("atomic");
    const RepositoryStreamKey first{"50000000-0000-4000-8000-000000000001",
                                    "60000000-0000-4000-8000-000000000001"};
    const RepositoryStreamKey second{"50000000-0000-4000-8000-000000000002",
                                     "60000000-0000-4000-8000-000000000002"};
    ProjectionEvent committed{first, 1, "persistent-global-event-0001", "persistent-epoch-0001",
                              "persistent-manifest-0001", {}};
    ProjectionEvent pending{second, 1, "persistent-pending-event-001", "persistent-epoch-0001",
                            "persistent-manifest-0001",
                            {{"file", "pending.cpp", ProjectionOperation::Upsert,
                              "persistent-digest-pending"}}};
    ProjectionEvent conflict{second, 2, committed.event_id, "persistent-epoch-0002",
                             std::nullopt, {}};
    {
        DuckdbPortfolioStore store(path);
        EXPECT_EQ(store.apply(first, 0, {committed}).disposition, ApplyDisposition::Applied);
        EXPECT_THROW(store.apply(second, 0, {pending, conflict}), PortfolioStoreError);
        EXPECT_FALSE(store.stream_state(second).exists);
    }
    {
        DuckdbPortfolioStore reopened(path);
        EXPECT_EQ(reopened.apply(first, 0, {committed}).disposition,
                  ApplyDisposition::Duplicate);
        EXPECT_EQ(reopened.apply(second, 0, {pending}).disposition,
                  ApplyDisposition::Applied);
        EXPECT_EQ(reopened.stream_state(second).cursor, 1u);
    }
    std::error_code error;
    fs::remove(path, error);
}

TEST(DuckdbPortfolioIntegration, ReconcileRepairsEpochAndModelsRemovalAndReturn) {
    const auto root = temporary_repository("reconcile");
    const auto central_path = temporary_database("reconcile-central");
    const auto stream = create_source(root, "70000000-0000-4000-8000-000000000001",
                                      "80000000-0000-4000-8000-000000000001");
    {
        DuckdbPortfolioStore store(central_path);
        DuckdbRepositoryProjector projector(store);
        EXPECT_EQ(projector.sync(root, root / ".axon/index.duckdb", 2).events_applied, 3u);
    }
    {
        duckdb::DuckDB database(central_path.string());
        duckdb::Connection connection(database);
        auto update = connection.Prepare(
            "UPDATE portfolio_streams SET epoch='corrupted-epoch-0001' "
            "WHERE repository_id=$1 AND index_stream_id=$2");
        ASSERT_FALSE(update->Execute(stream.repository_id, stream.index_stream_id)->HasError());
    }
    {
        DuckdbPortfolioStore store(central_path);
        DuckdbRepositoryProjector projector(store);
        EXPECT_TRUE(projector.sync(root, root / ".axon/index.duckdb").rebuilt);
        EXPECT_EQ(store.stream_state(stream).epoch, "source-epoch-0003");
        EXPECT_EQ(store.stream_state(stream).manifest, "source-manifest-snapshot");
    }
    {
        duckdb::DuckDB database(central_path.string());
        duckdb::Connection connection(database);
        ASSERT_FALSE(connection.Query(
            "UPDATE portfolio_streams SET manifest='corrupted-manifest-0001'")->HasError());
    }
    {
        DuckdbPortfolioStore store(central_path);
        DuckdbRepositoryProjector projector(store);
        EXPECT_TRUE(projector.sync(root, root / ".axon/index.duckdb").rebuilt);
        EXPECT_EQ(store.stream_state(stream).manifest, "source-manifest-snapshot");
        ProjectionEvent removed{stream, 4, "repository-removed-event-01",
                                "source-epoch-0004", std::nullopt,
                                {{"repository", stream.repository_id,
                                  ProjectionOperation::Delete, std::nullopt}}};
        EXPECT_TRUE(store.apply(stream, 3, {removed}).state.removed);
        ProjectionEvent returned{stream, 5, "repository-returned-event-01",
                                 "source-epoch-0005", std::nullopt,
                                 {{"repository", stream.repository_id,
                                   ProjectionOperation::Upsert,
                                   "repository-return-digest"}}};
        EXPECT_FALSE(store.apply(stream, 4, {returned}).state.removed);
    }
    std::error_code error;
    fs::remove_all(root, error);
    fs::remove(central_path, error);
}

TEST(DuckdbPortfolioIntegration, ProjectsReidentifiedJournalByPhysicalStreamWithoutOrphan) {
    const auto root = temporary_repository("reidentified");
    const auto central_path = temporary_database("reidentified-central");
    const auto previous = create_source(root, "90000000-0000-4000-8000-000000000001",
                                        "91000000-0000-4000-8000-000000000001");
    const auto current = reidentify_source(
        root, previous, "92000000-0000-4000-8000-000000000001");
    const auto source_path = root / ".axon/index.duckdb";
    const auto before = file_bytes(source_path);
    {
        DuckdbPortfolioStore store(central_path);
        DuckdbRepositoryProjector projector(store);
        const auto projected = projector.sync(root, source_path, 2);
        EXPECT_EQ(projected.stream, current);
        EXPECT_EQ(projected.cursor_after, 4u);
        EXPECT_EQ(projected.events_applied, 4u);
        EXPECT_FALSE(store.stream_state(previous).exists);
        const auto current_projection = store.inspect_repository_stream(current, 10);
        EXPECT_EQ(current_projection.state.cursor, 4u);
        EXPECT_TRUE(contains(current_projection, "file", "a.cpp"));
        EXPECT_TRUE(contains(current_projection, "repository", current.repository_id));
        EXPECT_FALSE(contains(current_projection, "repository", previous.repository_id));
        EXPECT_EQ(projector.sync(root, source_path, 2).events_applied, 0u);
    }
    EXPECT_EQ(file_bytes(source_path), before);
    {
        DuckdbPortfolioStore rebuilt;
        DuckdbRepositoryProjector rebuilder(rebuilt);
        EXPECT_TRUE(rebuilder.rebuild(root, source_path).rebuilt);
        EXPECT_FALSE(rebuilt.stream_state(previous).exists);
        EXPECT_EQ(rebuilt.stream_state(current).cursor, 4u);
        EXPECT_EQ(rebuilt.stream_state(current).epoch, "source-epoch-0004");
        EXPECT_TRUE(contains(rebuilt.inspect_repository_stream(current, 10), "repository",
                             current.repository_id));
    }
    {
        DuckdbPortfolioStore reopened(central_path);
        DuckdbRepositoryProjector projector(reopened);
        EXPECT_EQ(projector.sync(root, source_path, 1).cursor_after, 4u);
        EXPECT_FALSE(reopened.stream_state(previous).exists);
        EXPECT_EQ(reopened.stream_state(current).cursor, 4u);
    }
    std::error_code error;
    fs::remove_all(root, error);
    fs::remove(central_path, error);
}

TEST(DuckdbPortfolioIntegration, MalformedNestedJournalPayloadFailsBeforeBatchApply) {
    const auto root = temporary_repository("malformed");
    const auto stream = create_source(root, "93000000-0000-4000-8000-000000000001",
                                      "94000000-0000-4000-8000-000000000001");
    {
        duckdb::DuckDB database((root / ".axon/index.duckdb").string());
        duckdb::Connection connection(database);
        ASSERT_FALSE(connection.Query(
            "UPDATE index_events SET payload_json='{"
            "\"affected\":[{\"kind\":\"file\",\"key\":\"bad.cpp\","
            "\"operation\":\"upsert\",\"digest\":42}]}' WHERE sequence=2")
                         ->HasError());
    }
    const auto source_path = root / ".axon/index.duckdb";
    const auto before = file_bytes(source_path);
    DuckdbPortfolioStore store;
    DuckdbRepositoryProjector projector(store);
    EXPECT_THROW(projector.sync(root, source_path, 3), std::runtime_error);
    EXPECT_FALSE(store.stream_state(stream).exists);
    EXPECT_EQ(file_bytes(source_path), before);
    std::error_code error;
    fs::remove_all(root, error);
}

TEST(DuckdbPortfolioIntegration, SharesOneDatabaseHandleAcrossLiveStoresForSamePath) {
    const auto path = temporary_database("live-handles");
    ProjectionEvent first{kPersistentStream, 1, "live-handle-event-0001",
                          "live-handle-epoch-0001", "live-handle-manifest-0001",
                          {{"file", "a.cpp", ProjectionOperation::Upsert,
                            "live-handle-digest-a"}}};
    ProjectionEvent conflicting{kPersistentStream, 1, "live-handle-event-0002",
                                "live-handle-epoch-0001", "live-handle-manifest-0001",
                                {{"file", "b.cpp", ProjectionOperation::Upsert,
                                  "live-handle-digest-b"}}};
    {
        DuckdbPortfolioStore first_store(path);
        DuckdbPortfolioStore second_store(path);
        EXPECT_EQ(first_store.apply(kPersistentStream, 0, {first}).disposition,
                  ApplyDisposition::Applied);
        EXPECT_THROW(second_store.apply(kPersistentStream, 0, {conflicting}),
                     PortfolioStoreError);
        EXPECT_EQ(second_store.stream_state(kPersistentStream).cursor, 1u);
        EXPECT_TRUE(contains(second_store.inspect_repository_stream(kPersistentStream, 10),
                             "file", "a.cpp"));
        EXPECT_FALSE(contains(second_store.inspect_repository_stream(kPersistentStream, 10),
                              "file", "b.cpp"));
    }
    std::error_code error;
    fs::remove(path, error);
}

TEST(DuckdbPortfolioIntegration, RejectsMetadataIdentityDivergingFromJournalBeforeWrite) {
    const auto root = temporary_repository("identity-divergence");
    const auto previous = create_source(root, "95000000-0000-4000-8000-000000000001",
                                        "96000000-0000-4000-8000-000000000001");
    const auto current = reidentify_source(
        root, previous, "97000000-0000-4000-8000-000000000001");
    const RepositoryStreamKey divergent{"98000000-0000-4000-8000-000000000001",
                                        previous.index_stream_id};
    {
        duckdb::DuckDB database((root / ".axon/index.duckdb").string());
        duckdb::Connection connection(database);
        auto update = connection.Prepare(
            "UPDATE index_metadata SET repository_id=$1 WHERE singleton=true");
        ASSERT_FALSE(update->Execute(divergent.repository_id)->HasError());
    }
    DuckdbPortfolioStore store;
    DuckdbRepositoryProjector projector(store);
    EXPECT_THROW(projector.sync(root, root / ".axon/index.duckdb"), std::runtime_error);
    EXPECT_FALSE(store.stream_state(previous).exists);
    EXPECT_FALSE(store.stream_state(current).exists);
    EXPECT_FALSE(store.stream_state(divergent).exists);
    std::error_code error;
    fs::remove_all(root, error);
}

TEST(DuckdbPortfolioIntegration, RollsBackWholeOrdinarySourceBatchOnReceiptConflict) {
    const auto root = temporary_repository("source-batch-atomic");
    const auto source = create_source(root, "99000000-0000-4000-8000-000000000001",
                                      "9a000000-0000-4000-8000-000000000001");
    const RepositoryStreamKey other{"9b000000-0000-4000-8000-000000000001",
                                    "9c000000-0000-4000-8000-000000000001"};
    ProjectionEvent reservation{other, 1, "source-event-0002-" + source.repository_id,
                                "reserved-event-epoch-0001", "reserved-manifest-0001", {}};
    DuckdbPortfolioStore store;
    ASSERT_EQ(store.apply(other, 0, {reservation}).disposition, ApplyDisposition::Applied);
    DuckdbRepositoryProjector projector(store);
    EXPECT_THROW(projector.sync(root, root / ".axon/index.duckdb", 3),
                 PortfolioStoreError);
    EXPECT_FALSE(store.stream_state(source).exists);
    EXPECT_FALSE(contains(store.inspect_repository_stream(source, 10), "file", "a.cpp"));
    std::error_code error;
    fs::remove_all(root, error);
}

TEST(DuckdbPortfolioIntegration, RejectsNonCanonicalReidentificationBeforeWrite) {
    for (const bool add_extra : {false, true}) {
        const auto root = temporary_repository(add_extra ? "reidentity-extra" :
                                                        "reidentity-digest");
        const auto previous = create_source(
            root, add_extra ? "a1000000-0000-4000-8000-000000000001"
                            : "a2000000-0000-4000-8000-000000000001",
            add_extra ? "b1000000-0000-4000-8000-000000000001"
                      : "b2000000-0000-4000-8000-000000000001");
        const auto current = reidentify_source(
            root, previous,
            add_extra ? "c1000000-0000-4000-8000-000000000001"
                      : "c2000000-0000-4000-8000-000000000001",
            add_extra, !add_extra);
        DuckdbPortfolioStore incremental;
        DuckdbRepositoryProjector incremental_projector(incremental);
        EXPECT_THROW(incremental_projector.sync(root, root / ".axon/index.duckdb"),
                     std::runtime_error);
        EXPECT_FALSE(incremental.stream_state(previous).exists);
        EXPECT_FALSE(incremental.stream_state(current).exists);

        DuckdbPortfolioStore rebuilt;
        DuckdbRepositoryProjector rebuild_projector(rebuilt);
        EXPECT_THROW(rebuild_projector.rebuild(root, root / ".axon/index.duckdb"),
                     std::runtime_error);
        EXPECT_FALSE(rebuilt.stream_state(previous).exists);
        EXPECT_FALSE(rebuilt.stream_state(current).exists);
        std::error_code error;
        fs::remove_all(root, error);
    }
}

TEST(DuckdbPortfolioIntegration, RejectsEveryInvalidHandoffInvariantBeforeAnyWrite) {
    const std::vector<InvalidHandoff> defects = {
        InvalidHandoff::Reason,          InvalidHandoff::SameIdentity,
        InvalidHandoff::PreviousUuid,    InvalidHandoff::CurrentUuid,
        InvalidHandoff::StreamUuid,      InvalidHandoff::OldBindingShort,
        InvalidHandoff::NewBindingLong,  InvalidHandoff::ApprovalEmpty,
        InvalidHandoff::ApprovalLong,    InvalidHandoff::EventIdShort,
        InvalidHandoff::EpochShort,      InvalidHandoff::ManifestShort};
    std::size_t ordinal = 0;
    for (const auto defect : defects) {
        SCOPED_TRACE("invalid handoff invariant " + std::to_string(ordinal));
        const auto root = temporary_repository("invalid-handoff");
        const auto suffix = std::to_string(100000000000ull + ordinal);
        const auto previous = create_source(
            root, "d1000000-0000-4000-8000-" + suffix,
            "e1000000-0000-4000-8000-" + suffix);
        const auto current = reidentify_source(
            root, previous, "f1000000-0000-4000-8000-" + suffix);
        corrupt_handoff(root, previous, current, defect);

        DuckdbPortfolioStore store;
        DuckdbRepositoryProjector projector(store);
        EXPECT_THROW(projector.sync(root, root / ".axon/index.duckdb"), std::runtime_error);
        EXPECT_FALSE(store.stream_state(previous).exists);
        EXPECT_FALSE(store.stream_state(current).exists);
        std::error_code error;
        fs::remove_all(root, error);
        ++ordinal;
    }
}

} // namespace
