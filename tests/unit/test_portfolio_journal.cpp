#include "core/config.hpp"
#include "core/db.hpp"
#include "core/indexer.hpp"
#include "core/routes.hpp"
#include "portfolio/domain/index_journal.hpp"

#include <gtest/gtest.h>
#include <duckdb.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

constexpr const char* kRepositoryId = "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb";

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

int64_t scalar_i64(axon::Database& db, const std::string& sql) {
    auto result = db.conn().Query(sql);
    EXPECT_FALSE(result->HasError()) << result->GetError();
    return result->GetValue<int64_t>(0, 0);
}

std::string scalar_string(axon::Database& db, const std::string& sql) {
    auto result = db.conn().Query(sql);
    EXPECT_FALSE(result->HasError()) << result->GetError();
    return result->GetValue(0, 0).ToString();
}

class PortfolioJournalTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = fs::temp_directory_path() /
               ("axon-journal-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / ".axon");
        write_file(root / "repository-contract.yaml",
                   std::string("schema_version: repository-contract/v1\nrepository_id: ") +
                       kRepositoryId + "\n");
        write_file(root / "src/main.ts", "export function greet(name: string) { return name; }\n");
        cfg = axon::make_config(root);
    }

    void TearDown() override {
        axon::portfolio::clear_journal_failpoint_for_testing();
        std::error_code error;
        fs::remove_all(root, error);
    }

    fs::path root;
    axon::Config cfg;
};

TEST_F(PortfolioJournalTest, PersistsLogicalAndPhysicalIdentityAcrossUpgradeAndReopen) {
    std::string stream;
    {
        axon::Database db(cfg.db_path);
        const auto identity = axon::portfolio::index_identity(db.conn());
        EXPECT_EQ(identity.repository_id, kRepositoryId);
        EXPECT_EQ(identity.index_stream_id.size(), 36u);
        stream = identity.index_stream_id;
        EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM schema_migrations WHERE "
                                 "component='portfolio-index-journal' AND version=1"),
                  1);
    }
    axon::Database reopened(cfg.db_path);
    EXPECT_EQ(axon::portfolio::index_identity(reopened.conn()).index_stream_id, stream);
}

TEST_F(PortfolioJournalTest, UpgradesPreJournalDatabaseAdditivelyWithoutLosingRows) {
    {
        duckdb::DuckDB legacy(cfg.db_path.string());
        duckdb::Connection connection(legacy);
        auto created = connection.Query(
            "CREATE TABLE files(id BIGINT PRIMARY KEY,path VARCHAR UNIQUE,language VARCHAR,"
            "hash VARCHAR,indexed_at TIMESTAMP DEFAULT now(),byte_size BIGINT DEFAULT 0)");
        ASSERT_FALSE(created->HasError()) << created->GetError();
        auto inserted = connection.Query("INSERT INTO files(id,path,language,hash,byte_size) "
                                         "VALUES (7,'legacy.cpp','cpp','legacy-hash',42)");
        ASSERT_FALSE(inserted->HasError()) << inserted->GetError();
    }
    axon::Database upgraded(cfg.db_path);
    EXPECT_EQ(scalar_i64(upgraded, "SELECT COUNT(*) FROM files WHERE path='legacy.cpp'"), 1);
    EXPECT_EQ(scalar_i64(upgraded, "SELECT COUNT(*) FROM index_metadata"), 1);
    EXPECT_EQ(scalar_i64(upgraded, "SELECT COUNT(*) FROM index_events"), 0);
    EXPECT_EQ(axon::portfolio::index_identity(upgraded.conn()).repository_id, kRepositoryId);
    EXPECT_EQ(scalar_i64(upgraded, "SELECT COUNT(*) FROM external_dependencies"), 0);
}

TEST_F(PortfolioJournalTest, UnresolvedImportsAreBoundedDeduplicatedAndTransactional) {
    write_file(root / "src/main.ts", "import '@axon/contracts';\nimport '@axon/contracts';\n"
                                     "import './not-yet-created';\nexport const value = true;\n");
    axon::Database db(cfg.db_path);
    axon::index_project(cfg, db);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM external_dependencies"), 2);
    EXPECT_EQ(
        scalar_i64(db,
                   "SELECT COUNT(*) FROM external_dependencies WHERE specifier='@axon/contracts'"),
        1);

    write_file(root / "src/main.ts", "export const value = false;\n");
    axon::index_files(cfg, db, {root / "src/main.ts"}, false);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM external_dependencies"), 0);

    write_file(root / "src/main.ts",
               "import '@axon/rollback-check';\nexport const value = true;\n");
    axon::portfolio::set_journal_failpoint_for_testing("after_mutation");
    EXPECT_THROW(axon::index_files(cfg, db, {root / "src/main.ts"}, false), std::runtime_error);
    axon::portfolio::clear_journal_failpoint_for_testing();
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM external_dependencies"), 0);

    axon::index_files(cfg, db, {root / "src/main.ts"}, false);
    EXPECT_EQ(
        scalar_i64(
            db,
            "SELECT COUNT(*) FROM external_dependencies WHERE specifier='@axon/rollback-check'"),
        1);
    fs::remove(root / "src/main.ts");
    axon::index_files(cfg, db, {}, true);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM external_dependencies"), 0);
}

TEST_F(PortfolioJournalTest, RejectsOversizedUnresolvedImportSpecifierWithoutPartialWrite) {
    const std::string oversized(1025, 'x');
    write_file(root / "src/main.ts", "import '" + oversized + "';\nexport const value = true;\n");
    axon::Database db(cfg.db_path);
    EXPECT_THROW(axon::index_project(cfg, db), std::invalid_argument);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM files"), 0);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM external_dependencies"), 0);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM index_events"), 0);
}

TEST_F(PortfolioJournalTest, FreshCloneGetsDistinctStreamWithSameLogicalRepository) {
    axon::Database first(cfg.db_path);
    const auto first_identity = axon::portfolio::index_identity(first.conn());

    const fs::path clone = root.parent_path() / (root.filename().string() + "-clone");
    fs::create_directories(clone / ".axon");
    write_file(clone / "repository-contract.yaml",
               std::string("schema_version: repository-contract/v1\nrepository_id: ") +
                   kRepositoryId + "\nidentity:\n  remotes: []\n");
    axon::Database second(clone / ".axon/index.duckdb");
    const auto second_identity = axon::portfolio::index_identity(second.conn());
    EXPECT_EQ(second_identity.repository_id, first_identity.repository_id);
    EXPECT_NE(second_identity.index_stream_id, first_identity.index_stream_id);
    std::error_code error;
    fs::remove_all(clone, error);
}

TEST_F(PortfolioJournalTest, InvalidPresentRepositoryContractFailsClosed) {
    const fs::path invalid = root.parent_path() / (root.filename().string() + "-invalid-contract");
    fs::create_directories(invalid / ".axon");
    write_file(invalid / "repository-contract.yaml", "repository_id: definitely-not-a-uuid\n");
    EXPECT_THROW(axon::Database(invalid / ".axon/index.duckdb"), std::runtime_error);
    std::error_code error;
    fs::remove_all(invalid, error);
}

TEST_F(PortfolioJournalTest, StructurallyInvalidIdentityEnvelopeFailsClosed) {
    const fs::path invalid = root.parent_path() / (root.filename().string() + "-invalid-envelope");
    fs::create_directories(invalid / ".axon");
    write_file(invalid / "repository-contract.yaml",
               std::string("schema_version: wrong/v99\nrepository_id: ") + kRepositoryId +
                   "\nidentity: definitely-not-an-object\n");
    EXPECT_THROW(axon::Database(invalid / ".axon/index.duckdb"), std::runtime_error);

    write_file(invalid / "repository-contract.yaml",
               std::string("schema_version: repository-contract/v1\nrepository_id: ") +
                   kRepositoryId + "\nidentity: definitely-not-an-object\n");
    EXPECT_THROW(axon::Database(invalid / ".axon/index.duckdb"), std::runtime_error);
    std::error_code error;
    fs::remove_all(invalid, error);
}

TEST_F(PortfolioJournalTest, ReopenFailsClosedAfterContractBecomesInvalidOrDiverges) {
    {
        axon::Database db(cfg.db_path);
        EXPECT_EQ(axon::portfolio::index_identity(db.conn()).repository_id, kRepositoryId);
    }

    write_file(root / "repository-contract.yaml", "schema_version: repository-contract/v1\n"
                                                  "repository_id: definitely-not-a-uuid\n");
    EXPECT_THROW(axon::Database(cfg.db_path), std::runtime_error);

    write_file(root / "repository-contract.yaml",
               "schema_version: repository-contract/v1\n"
               "repository_id: 853c3996-ea2d-4836-b0f7-872e39d771f4\n");
    EXPECT_THROW(axon::Database(cfg.db_path), std::runtime_error);
}

TEST_F(PortfolioJournalTest, FullIncrementalDeleteAndRoutesAreJournaledWithTombstones) {
    axon::Database db(cfg.db_path);
    auto full = axon::index_project(cfg, db);
    EXPECT_EQ(full.files_indexed, 1);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM index_events WHERE "
                             "event_type='IndexSnapshotCompleted'"),
              1);
    EXPECT_EQ(scalar_string(db, "SELECT manifest_hash FROM index_events WHERE "
                                "event_type='IndexSnapshotCompleted'"),
              axon::portfolio::compute_manifest_hash(db.conn()));

    write_file(root / "src/main.ts",
               "export function greet(name: string) { return `hello ${name}`; }\n");
    auto incremental = axon::index_files(cfg, db, {root / "src/main.ts"}, false);
    EXPECT_EQ(incremental.files_indexed, 1);
    EXPECT_GE(scalar_i64(db, "SELECT COUNT(*) FROM index_events WHERE "
                             "event_type='IndexFilesUpdated'"),
              2);

    write_file(root / "src/api.ts", "router.get('/health', handler);\n");
    axon::index_files(cfg, db, {root / "src/api.ts"}, false);
    EXPECT_EQ(axon::index_routes(cfg, db), 1);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM index_events WHERE "
                             "event_type='IndexRoutesUpdated'"),
              1);
    write_file(root / "src/api.ts", "export const noRoute = true;\n");
    EXPECT_EQ(axon::index_routes(cfg, db), 0);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM index_tombstones WHERE "
                             "entity_kind='route' AND entity_key='GET /health@src/api.ts'"),
              1);

    fs::remove(root / "src/main.ts");
    auto deleted = axon::index_files(cfg, db, {}, true);
    EXPECT_EQ(deleted.files_pruned, 1);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM index_tombstones WHERE "
                             "entity_kind='file' AND entity_key='src/main.ts'"),
              1);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM files f LEFT JOIN index_events e ON true "
                             "WHERE f.path='src/main.ts'"),
              0);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM index_events e LEFT JOIN index_metadata m "
                             "ON e.index_stream_id=m.index_stream_id WHERE e.sequence < 1"),
              0);

    write_file(root / "src/main.ts", "export const restored = true;\n");
    axon::index_files(cfg, db, {root / "src/main.ts"}, false);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM index_tombstones WHERE "
                             "entity_kind='file' AND entity_key='src/main.ts'"),
              0);
}

TEST_F(PortfolioJournalTest, CapabilityEvidenceIsNormalizedBackfilledAndPrunedTransactionally) {
    write_file(root / "src/billing/payment.ts",
               "// initial comment\nexport function authorize() { return true; }\n");
    axon::Database db(cfg.db_path);
    axon::index_project(cfg, db);
    const auto payment_id =
        scalar_i64(db, "SELECT id FROM files WHERE path='src/billing/payment.ts'");
    EXPECT_EQ(scalar_string(db, "SELECT bounded_context FROM capability_contexts WHERE file_id=" +
                                    std::to_string(payment_id)),
              "billing");
    const auto original =
        scalar_string(db, "SELECT value FROM capability_ast_fingerprints WHERE file_id=" +
                              std::to_string(payment_id));
    EXPECT_EQ(original.size(), 64U);

    write_file(root / "src/billing/payment.ts",
               "// rewritten comment\nexport function authorize() { return true; }\n");
    axon::index_files(cfg, db, {root / "src/billing/payment.ts"}, false);
    EXPECT_EQ(scalar_string(db, "SELECT value FROM capability_ast_fingerprints WHERE file_id=" +
                                    std::to_string(payment_id)),
              original);

    db.exec("DELETE FROM capability_contexts");
    db.exec("DELETE FROM capability_ast_fingerprints");
    axon::index_project(cfg, db);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM capability_ast_fingerprints WHERE file_id=" +
                                 std::to_string(payment_id)),
              1);
    EXPECT_GE(
        scalar_i64(db, "SELECT COUNT(*) FROM index_events WHERE event_type='IndexFilesUpdated'"),
        2);

    fs::remove(root / "src/billing/payment.ts");
    axon::index_files(cfg, db, {}, true);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM capability_contexts WHERE file_id=" +
                                 std::to_string(payment_id)),
              0);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM capability_ast_fingerprints WHERE file_id=" +
                                 std::to_string(payment_id)),
              0);
}

TEST_F(PortfolioJournalTest, BackfillUsesTheSameContextInferenceForNonWrapperPaths) {
    write_file(root / "billing/payment.ts", "export function authorize() { return true; }\n");
    axon::Database db(cfg.db_path);
    axon::index_project(cfg, db);
    const auto id = scalar_i64(db, "SELECT id FROM files WHERE path='billing/payment.ts'");
    db.exec("DELETE FROM capability_contexts WHERE file_id=" + std::to_string(id));
    const auto events = scalar_i64(db, "SELECT COUNT(*) FROM index_events");
    EXPECT_EQ(axon::index_project(cfg, db).files_indexed, 0);
    EXPECT_EQ(scalar_string(db, "SELECT bounded_context FROM capability_contexts WHERE file_id=" +
                                    std::to_string(id)),
              "billing");
    EXPECT_GT(scalar_i64(db, "SELECT COUNT(*) FROM index_events"), events);
}

TEST_F(PortfolioJournalTest, FailpointsProveRollbackBeforeCommitAndDurabilityAfterCommit) {
    axon::Database db(cfg.db_path);
    axon::index_project(cfg, db);
    const int64_t events_before = scalar_i64(db, "SELECT COUNT(*) FROM index_events");
    const std::string hash_before =
        scalar_string(db, "SELECT hash FROM files WHERE path='src/main.ts'");

    write_file(root / "src/main.ts", "export const changed = true;\n");
    for (const std::string failpoint : {"after_mutation", "after_event", "before_commit"}) {
        axon::portfolio::set_journal_failpoint_for_testing(failpoint);
        EXPECT_THROW(axon::index_files(cfg, db, {root / "src/main.ts"}, false), std::runtime_error);
        axon::portfolio::clear_journal_failpoint_for_testing();
        EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM index_events"), events_before);
        EXPECT_EQ(scalar_string(db, "SELECT hash FROM files WHERE path='src/main.ts'"),
                  hash_before);
    }

    axon::portfolio::set_journal_failpoint_for_testing("after_commit");
    EXPECT_THROW(axon::index_files(cfg, db, {root / "src/main.ts"}, false), std::runtime_error);
    axon::portfolio::clear_journal_failpoint_for_testing();
    EXPECT_GT(scalar_i64(db, "SELECT COUNT(*) FROM index_events"), events_before);
    EXPECT_NE(scalar_string(db, "SELECT hash FROM files WHERE path='src/main.ts'"), hash_before);
}

TEST_F(PortfolioJournalTest, ManifestIsDeterministicAndPayloadContainsNoSourceBody) {
    axon::Database db(cfg.db_path);
    axon::index_project(cfg, db);
    const std::string first = axon::portfolio::compute_manifest_hash(db.conn());
    const std::string second = axon::portfolio::compute_manifest_hash(db.conn());
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.size(), 64u);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM index_events WHERE "
                             "payload_json LIKE '%export function%'"),
              0);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM (SELECT sequence, lag(sequence) OVER "
                             "(ORDER BY sequence) previous FROM index_events) q "
                             "WHERE previous IS NOT NULL AND sequence <= previous"),
              0);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM (SELECT sequence,row_number() OVER "
                             "(ORDER BY sequence) expected FROM index_events) q "
                             "WHERE sequence<>expected"),
              0);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM index_events WHERE "
                             "schema_version<>'axon/index-event/v1' OR length(event_id)<>64"),
              0);
    EXPECT_EQ(scalar_string(db, "SELECT current_epoch FROM index_metadata"),
              scalar_string(db, "SELECT index_epoch FROM index_events ORDER BY sequence DESC "
                                "LIMIT 1"));
}

TEST_F(PortfolioJournalTest, EmbeddingVectorChangesManifestWithoutSourceMutation) {
    axon::Database db(cfg.db_path);
    axon::index_project(cfg, db);
    const std::string before = axon::portfolio::compute_manifest_hash(db.conn());
    std::string vector = "[1";
    for (int i = 1; i < 768; ++i)
        vector += ",0";
    vector += "]::FLOAT[768]";
    auto updated = db.conn().Query("UPDATE symbols SET embedding=" + vector +
                                   " WHERE id=(SELECT "
                                   "MIN(id) FROM symbols)");
    ASSERT_FALSE(updated->HasError()) << updated->GetError();
    const std::string after = axon::portfolio::compute_manifest_hash(db.conn());
    EXPECT_NE(before, after);
    EXPECT_EQ(after, axon::portfolio::compute_manifest_hash(db.conn()));
}

TEST_F(PortfolioJournalTest, EventSchemaEnumsAndDeleteConstraintsFailClosed) {
    axon::Database db(cfg.db_path);
    axon::portfolio::Transaction transaction(db.conn());
    const std::string manifest = axon::portfolio::compute_manifest_hash(db.conn());
    EXPECT_THROW(
        axon::portfolio::append_index_event(transaction, db.conn(), "MadeUpEvent", {}, manifest),
        std::invalid_argument);
    EXPECT_THROW(axon::portfolio::append_index_event(transaction, db.conn(), "IndexFilesUpdated",
                                                     {{"made-up", "x", "upsert", std::nullopt}},
                                                     manifest),
                 std::invalid_argument);
    EXPECT_THROW(axon::portfolio::append_index_event(transaction, db.conn(), "IndexFilesUpdated",
                                                     {{"file", "x", "made-up", std::nullopt}},
                                                     manifest),
                 std::invalid_argument);
    EXPECT_THROW(axon::portfolio::append_index_event(transaction, db.conn(), "IndexFilesDeleted",
                                                     {{"file", "x", "upsert", std::nullopt}},
                                                     manifest),
                 std::invalid_argument);
    EXPECT_THROW(axon::portfolio::append_index_event(transaction, db.conn(),
                                                     "RepositoryReidentified", {}, manifest),
                 std::invalid_argument);
    EXPECT_THROW(axon::portfolio::append_index_event(
                     transaction, db.conn(), "IndexSnapshotCompleted",
                     {{"repository", kRepositoryId, "snapshot", std::nullopt}}, ""),
                 std::invalid_argument);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM index_events"), 0);
    transaction.mark_index_mutation();
    EXPECT_THROW(transaction.commit(), std::logic_error);
}

TEST_F(PortfolioJournalTest, ReidentificationAndRemovalAreTypedAtomicAndSequenceContinuous) {
    axon::Database db(cfg.db_path);
    axon::index_project(cfg, db);
    const auto before = axon::portfolio::index_identity(db.conn());
    const std::string replacement = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    {
        axon::portfolio::Transaction transaction(db.conn());
        axon::portfolio::IdentityChange change = {before.repository_id,     replacement,
                                                  "old-binding-0001",       "new-binding-0001",
                                                  "owner-approval:G3-test", "owner-approved"};
        EXPECT_GT(axon::portfolio::reidentify_repository(transaction, db.conn(), change), 0u);
        transaction.commit();
    }
    const auto rebound = axon::portfolio::index_identity(db.conn());
    EXPECT_EQ(rebound.repository_id, replacement);
    EXPECT_EQ(rebound.index_stream_id, before.index_stream_id);
    EXPECT_FALSE(rebound.removed);
    EXPECT_NE(scalar_string(db, "SELECT payload_json FROM index_events WHERE "
                                "event_type='RepositoryReidentified'")
                  .find("owner-approval:G3-test"),
              std::string::npos);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM index_events WHERE "
                             "event_type='RepositoryReidentified' AND "
                             "repository_id='7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb'"),
              1);
    {
        axon::portfolio::Transaction transaction(db.conn());
        EXPECT_GT(axon::portfolio::remove_repository(transaction, db.conn(), "owner-remove:G3"),
                  0u);
        transaction.commit();
    }
    EXPECT_EQ(scalar_i64(db, "SELECT CAST(removed AS BIGINT) FROM index_metadata"), 1);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM index_tombstones WHERE "
                             "entity_kind='repository' AND entity_key='" +
                                 replacement + "'"),
              1);
    write_file(root / "src/main.ts", "export const forbiddenAfterRemoval = true;\n");
    EXPECT_THROW(axon::index_files(cfg, db, {root / "src/main.ts"}, false), std::logic_error);
    EXPECT_EQ(scalar_i64(db, "SELECT COUNT(*) FROM (SELECT sequence,row_number() OVER "
                             "(ORDER BY sequence) expected FROM index_events) q "
                             "WHERE sequence<>expected"),
              0);
}

TEST_F(PortfolioJournalTest, SymbolModeCallResolutionStaysInsideOuterJournalTransaction) {
    cfg.project_cfg.granularity = "symbol";
    write_file(root / "src/helper.ts", "export function helper(value: string) { return value; }\n");
    write_file(root / "src/main.ts", "import { helper } from './helper';\n"
                                     "export function caller() { return helper('ok'); }\n");
    axon::Database db(cfg.db_path);
    EXPECT_NO_THROW(axon::index_project(cfg, db));
    EXPECT_GT(scalar_i64(db, "SELECT COUNT(*) FROM edges WHERE kind='calls'"), 0);
    EXPECT_GT(scalar_i64(db, "SELECT COUNT(*) FROM index_events"), 0);
}

} // namespace
