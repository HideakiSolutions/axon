#include "portfolio/infrastructure/postgresql/postgresql_portfolio_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <string>

namespace {

using namespace axon::portfolio;

const RepositoryStreamKey kStream{"2159f9cf-c2e0-4a61-ab7b-a5fd0918cbbb",
                                  "2b809f2e-5606-4f45-b050-e4dbb30cde53"};
const RepositoryStreamKey kOther{"3159f9cf-c2e0-4a61-ab7b-a5fd0918cbbb",
                                 "3b809f2e-5606-4f45-b050-e4dbb30cde53"};

std::string dsn() {
    const auto* value = std::getenv("AXON_POSTGRES_TEST_DSN");
    if (!value || std::string(value).empty())
        throw std::runtime_error("AXON_POSTGRES_TEST_DSN is required");
    return value;
}

std::string schema(const char* suffix) {
    static std::atomic<std::uint64_t> ordinal{0};
    return std::string("axon_test_g6_") + suffix + "_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
           std::to_string(ordinal.fetch_add(1));
}

ProjectionEvent event(const RepositoryStreamKey& stream, std::uint64_t sequence,
                      const std::string& event_id, const std::string& key) {
    return {stream,
            sequence,
            event_id,
            "postgres-epoch-0001",
            "postgres-manifest-0001",
            {{"file", key, ProjectionOperation::Upsert, "postgres-digest-0001"}}};
}

bool contains(const StreamProjection& projection, const std::string& key) {
    return std::any_of(projection.entities.begin(), projection.entities.end(),
                       [&](const auto& entity) { return entity.entity_key == key; });
}

TEST(PostgresqlPortfolioIntegration, PersistsProjectionAndDurableOutboxAcrossConnections) {
    const auto namespace_name = schema("persist");
    {
        PostgresqlPortfolioStore store(dsn(), namespace_name, true);
        const auto applied = store.apply(
            kStream, 0, {event(kStream, 1, "postgres-persist-event-0001", "persist.cpp")});
        EXPECT_EQ(applied.state.cursor, 1u);
        EXPECT_EQ(store.pending_outbox_count(), 1u);
        PostgresqlPortfolioStore reopened(dsn(), namespace_name);
        EXPECT_EQ(reopened.stream_state(kStream).cursor, 1u);
        EXPECT_TRUE(contains(reopened.inspect_repository_stream(kStream, 10), "persist.cpp"));
        EXPECT_EQ(reopened.pending_outbox_count(), 1u);
        EXPECT_EQ(reopened
                      .apply(kStream, 0,
                             {event(kStream, 1, "postgres-persist-event-0001", "persist.cpp")})
                      .disposition,
                  ApplyDisposition::Duplicate);
        EXPECT_EQ(reopened.pending_outbox_count(), 1u);
    }
}

TEST(PostgresqlPortfolioIntegration, CompetingClientsCannotBothCommitSameCursor) {
    const auto namespace_name = schema("concurrent");
    PostgresqlPortfolioStore first(dsn(), namespace_name, true);
    PostgresqlPortfolioStore second(dsn(), namespace_name);
    std::atomic<int> applied{0};
    std::atomic<int> rejected{0};
    auto writer = [&](PostgresqlPortfolioStore& store, const char* event_id, const char* key) {
        try {
            if (store.apply(kStream, 0, {event(kStream, 1, event_id, key)}).disposition ==
                ApplyDisposition::Applied)
                ++applied;
        } catch (const PortfolioStoreError&) {
            ++rejected;
        }
    };
    auto left = std::async(std::launch::async, writer, std::ref(first),
                           "postgres-concurrent-event-a", "a.cpp");
    auto right = std::async(std::launch::async, writer, std::ref(second),
                            "postgres-concurrent-event-b", "b.cpp");
    left.get();
    right.get();
    EXPECT_EQ(applied, 1);
    EXPECT_EQ(rejected, 1);
    PostgresqlPortfolioStore inspect(dsn(), namespace_name);
    EXPECT_EQ(inspect.stream_state(kStream).cursor, 1u);
    EXPECT_EQ(inspect.inspect_repository_stream(kStream, 10).entities.size(), 1u);
    EXPECT_EQ(inspect.pending_outbox_count(), 1u);
}

TEST(PostgresqlPortfolioIntegration, ReceiptConflictRollsBackBatchCursorEntitiesAndOutbox) {
    const auto namespace_name = schema("rollback");
    PostgresqlPortfolioStore store(dsn(), namespace_name, true);
    const auto reserved = event(kOther, 1, "postgres-shared-event-0001", "reserved.cpp");
    ASSERT_EQ(store.apply(kOther, 0, {reserved}).disposition, ApplyDisposition::Applied);
    const auto first = event(kStream, 1, "postgres-pending-event-0001", "pending.cpp");
    const auto conflict = event(kStream, 2, reserved.event_id, "conflict.cpp");
    EXPECT_THROW(store.apply(kStream, 0, {first, conflict}), PortfolioStoreError);
    EXPECT_FALSE(store.stream_state(kStream).exists);
    EXPECT_TRUE(store.inspect_repository_stream(kStream, 10).entities.empty());
    EXPECT_EQ(store.pending_outbox_count(), 1u);
}

TEST(PostgresqlPortfolioIntegration, ReplaceOutboxTracksStateCyclesButNotImmediateReplay) {
    PostgresqlPortfolioStore store(dsn(), schema("replace_outbox"), true);
    ASSERT_EQ(store.apply(kStream, 0, {event(kStream, 1, "postgres-replace-seed-0001", "seed.cpp")})
                  .disposition,
              ApplyDisposition::Applied);
    RepositorySnapshot first{
        kStream,
        1,
        "postgres-epoch-0002",
        "postgres-manifest-0002",
        false,
        false,
        {{"file", "first.cpp", ProjectionOperation::Upsert, "postgres-digest-first"}}};
    RepositorySnapshot second{
        kStream,
        1,
        "postgres-epoch-0003",
        "postgres-manifest-0003",
        false,
        false,
        {{"file", "second.cpp", ProjectionOperation::Upsert, "postgres-digest-second"}}};
    EXPECT_EQ(store.replace_repository_stream(first, 1).disposition, ApplyDisposition::Applied);
    EXPECT_EQ(store.replace_repository_stream(second, 1).disposition, ApplyDisposition::Applied);
    EXPECT_EQ(store.replace_repository_stream(first, 1).disposition, ApplyDisposition::Applied);
    EXPECT_EQ(store.replace_repository_stream(first, 1).disposition, ApplyDisposition::Duplicate);
    EXPECT_EQ(store.pending_outbox_count(), 4u);
}

TEST(PostgresqlPortfolioIntegration, RejectsInvalidSchemaIdentifierBeforeConnectionMutation) {
    EXPECT_THROW(PostgresqlPortfolioStore(dsn(), "public;drop_schema"), std::invalid_argument);
    EXPECT_THROW(PostgresqlPortfolioStore(dsn(), "UpperCase"), std::invalid_argument);
    EXPECT_THROW(PostgresqlPortfolioStore(dsn(), "1leading_digit"), std::invalid_argument);
    const std::string unavailable = "host=127.0.0.1 port=1 connect_timeout=1";
    EXPECT_THROW(PostgresqlPortfolioStore(unavailable, "public", true), std::invalid_argument);
    EXPECT_THROW(PostgresqlPortfolioStore(unavailable, "information_schema", true),
                 std::invalid_argument);
    EXPECT_THROW(PostgresqlPortfolioStore(unavailable, "pg_temp_attack", true),
                 std::invalid_argument);
    EXPECT_THROW(PostgresqlPortfolioStore(unavailable, "axon_runtime", true),
                 std::invalid_argument);
}

TEST(PostgresqlPortfolioIntegration, ChecksumMismatchFailsBeforeSchemaExpansion) {
    const auto namespace_name = schema("checksum");
    auto* connection = PQconnectdb(dsn().c_str());
    ASSERT_NE(connection, nullptr);
    ASSERT_EQ(PQstatus(connection), CONNECTION_OK);
    auto run = [&](const std::string& sql) {
        auto* result = PQexec(connection, sql.c_str());
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(PQresultStatus(result), PGRES_COMMAND_OK);
        PQclear(result);
    };
    run("CREATE SCHEMA " + namespace_name);
    run("CREATE TABLE " + namespace_name +
        ".portfolio_schema_migrations(version integer PRIMARY KEY,checksum text NOT NULL)");
    run("INSERT INTO " + namespace_name +
        ".portfolio_schema_migrations VALUES(1,'wrong-checksum')");
    EXPECT_THROW(PostgresqlPortfolioStore(dsn(), namespace_name), PortfolioStoreError);
    auto* result =
        PQexec(connection,
               ("SELECT to_regclass('" + namespace_name + ".portfolio_streams') IS NULL").c_str());
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(PQresultStatus(result), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(result, 0, 0), "t");
    PQclear(result);
    run("DROP SCHEMA " + namespace_name + " CASCADE");
    PQfinish(connection);
}

TEST(PostgresqlPortfolioIntegration, UpgradesValidV1SchemaAdditivelyAndPreservesRows) {
    const auto namespace_name = schema("upgrade");
    auto* connection = PQconnectdb(dsn().c_str());
    ASSERT_NE(connection, nullptr);
    ASSERT_EQ(PQstatus(connection), CONNECTION_OK);
    auto run = [&](const std::string& sql) {
        auto* result = PQexec(connection, sql.c_str());
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(PQresultStatus(result), PGRES_COMMAND_OK);
        PQclear(result);
    };
    run("CREATE SCHEMA " + namespace_name);
    run("CREATE TABLE " + namespace_name +
        ".portfolio_schema_migrations(version integer PRIMARY KEY,checksum text NOT NULL)");
    run("INSERT INTO " + namespace_name +
        ".portfolio_schema_migrations VALUES(1,'axon/portfolio-postgresql/v1')");
    run("CREATE TABLE " + namespace_name +
        ".portfolio_streams(repository_id text NOT NULL,index_stream_id text NOT NULL UNIQUE,"
        "cursor numeric(20,0) NOT NULL,epoch text NOT NULL,manifest text NOT NULL,stale boolean "
        "NOT NULL,removed boolean NOT NULL,PRIMARY KEY(repository_id,index_stream_id))");
    run("CREATE TABLE " + namespace_name +
        ".portfolio_entities(repository_id text NOT NULL,index_stream_id text NOT NULL,entity_kind "
        "text NOT NULL,entity_key text NOT NULL,digest text,PRIMARY "
        "KEY(repository_id,index_stream_id,"
        "entity_kind,entity_key))");
    run("CREATE TABLE " + namespace_name +
        ".portfolio_events(event_id text PRIMARY KEY,repository_id text NOT NULL,index_stream_id "
        "text NOT NULL,sequence numeric(20,0) NOT NULL,fingerprint text NOT "
        "NULL,UNIQUE(index_stream_id,sequence))");
    run("CREATE TABLE " + namespace_name + ".legacy_marker(value text NOT NULL)");
    run("INSERT INTO " + namespace_name + ".legacy_marker VALUES('preserved')");
    {
        PostgresqlPortfolioStore upgraded(dsn(), namespace_name, true);
        EXPECT_EQ(upgraded.pending_outbox_count(), 0u);
        auto* result = PQexec(
            connection,
            ("SELECT count(*) FROM " + namespace_name + ".portfolio_schema_migrations").c_str());
        ASSERT_NE(result, nullptr);
        ASSERT_EQ(PQresultStatus(result), PGRES_TUPLES_OK);
        EXPECT_STREQ(PQgetvalue(result, 0, 0), "2");
        PQclear(result);
        result =
            PQexec(connection, ("SELECT value FROM " + namespace_name + ".legacy_marker").c_str());
        ASSERT_NE(result, nullptr);
        ASSERT_EQ(PQresultStatus(result), PGRES_TUPLES_OK);
        EXPECT_STREQ(PQgetvalue(result, 0, 0), "preserved");
        PQclear(result);
    }
    run("DROP SCHEMA " + namespace_name + " CASCADE");
    PQfinish(connection);
}

TEST(PostgresqlPortfolioIntegration, NeverDropsPreexistingDisposableNamedSchema) {
    const auto namespace_name = schema("preexisting");
    auto* connection = PQconnectdb(dsn().c_str());
    ASSERT_NE(connection, nullptr);
    ASSERT_EQ(PQstatus(connection), CONNECTION_OK);
    auto run = [&](const std::string& sql) {
        auto* result = PQexec(connection, sql.c_str());
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(PQresultStatus(result), PGRES_COMMAND_OK);
        PQclear(result);
    };
    run("CREATE SCHEMA " + namespace_name);
    run("CREATE TABLE " + namespace_name + ".owner_marker(value text NOT NULL)");
    run("INSERT INTO " + namespace_name + ".owner_marker VALUES('external')");
    {
        PostgresqlPortfolioStore non_owner(dsn(), namespace_name, true);
        EXPECT_EQ(non_owner.pending_outbox_count(), 0u);
    }
    auto* result =
        PQexec(connection, ("SELECT value FROM " + namespace_name + ".owner_marker").c_str());
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(PQresultStatus(result), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(result, 0, 0), "external");
    PQclear(result);
    run("DROP SCHEMA " + namespace_name + " CASCADE");
    PQfinish(connection);
}

} // namespace
