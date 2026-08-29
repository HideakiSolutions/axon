#include <gtest/gtest.h>
#include "core/db.hpp"
#include "core/dialogue.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ── Fixture: in-memory DuckDB via temp file ───────────────────────────────────

static fs::path make_temp_db() {
    static int counter = 0;
    auto p = fs::temp_directory_path() / ("axon_dialogue_test_" + std::to_string(::getpid()) + "_" +
                                          std::to_string(++counter) + ".duckdb");
    return p;
}

class DialogueTest : public ::testing::Test {
protected:
    fs::path db_path;
    std::unique_ptr<axon::Database> db;

    void SetUp() override {
        db_path = make_temp_db();
        db = std::make_unique<axon::Database>(db_path);
    }

    void TearDown() override {
        db.reset();
        fs::remove(db_path);
    }
};

TEST(DialogueMigrationTest, LegacyDatabaseReceivesAdditiveMemorySchema) {
    const fs::path db_path = make_temp_db();
    {
        duckdb::DuckDB legacy(db_path.string());
        duckdb::Connection connection(legacy);
        auto observations = connection.Query(
            "CREATE TABLE observations (id BIGINT PRIMARY KEY, content VARCHAR NOT NULL, "
            "file_path VARCHAR, embedding FLOAT[768], created_at TIMESTAMP DEFAULT now())");
        ASSERT_FALSE(observations->HasError()) << observations->GetError();
        auto insert = connection.Query(
            "INSERT INTO observations(id, content) VALUES (41, 'legacy observation')");
        ASSERT_FALSE(insert->HasError()) << insert->GetError();
        auto sessions = connection.Query(
            "CREATE TABLE sessions (id BIGINT PRIMARY KEY, thread_id BIGINT NOT NULL, "
            "label VARCHAR, started_at TIMESTAMP DEFAULT now(), ended_at TIMESTAMP, "
            "digest VARCHAR, digest_embedding FLOAT[768])");
        ASSERT_FALSE(sessions->HasError()) << sessions->GetError();
    }

    {
        axon::Database migrated(db_path);
        auto authority = migrated.conn().Query("SELECT authority FROM observations WHERE id = 41");
        ASSERT_FALSE(authority->HasError()) << authority->GetError();
        ASSERT_EQ(authority->RowCount(), 1u);
        EXPECT_DOUBLE_EQ(authority->GetValue<double>(0, 0), 1.0);

        const int64_t thread_id = axon::thread_create(migrated, "migrated-thread");
        const int64_t first = axon::session_start(migrated, thread_id, "legacy", "retry-key");
        EXPECT_EQ(first, axon::session_start(migrated, thread_id, "replay", "retry-key"));

        const int64_t handoff_id = axon::handoff_create(migrated, first, "worker", "/repo", "/repo",
                                                        "Continue migrated work");
        EXPECT_EQ(axon::handoff_get(migrated, handoff_id).status, "pending");
    }
    fs::remove(db_path);
}

// ── Thread CRUD ───────────────────────────────────────────────────────────────

TEST_F(DialogueTest, ThreadCreateAndList) {
    int64_t id = axon::thread_create(*db, "auth-module", "project");
    EXPECT_GT(id, 0);

    auto threads = axon::thread_list(*db);
    ASSERT_EQ(threads.size(), 1u);
    EXPECT_EQ(threads[0].name, "auth-module");
    EXPECT_EQ(threads[0].kind, "project");
    EXPECT_EQ(threads[0].id, id);
}

TEST_F(DialogueTest, ThreadCreateIdempotent) {
    int64_t id1 = axon::thread_create(*db, "dup-thread");
    int64_t id2 = axon::thread_create(*db, "dup-thread");
    EXPECT_EQ(id1, id2);
}

TEST_F(DialogueTest, ThreadMultipleKinds) {
    axon::thread_create(*db, "alice", "person");
    axon::thread_create(*db, "project-x", "project");
    axon::thread_create(*db, "perf", "topic");

    auto threads = axon::thread_list(*db);
    EXPECT_EQ(threads.size(), 3u);
}

// ── Session operations ────────────────────────────────────────────────────────

TEST_F(DialogueTest, SessionStartAndGet) {
    int64_t tid = axon::thread_create(*db, "my-thread");
    int64_t sid = axon::session_start(*db, tid, "Sprint 1 planning");
    EXPECT_GT(sid, 0);

    auto sessions = axon::thread_get_sessions(*db, tid);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].label, "Sprint 1 planning");
    EXPECT_EQ(sessions[0].thread_id, tid);
}

TEST_F(DialogueTest, SessionEndMarksEndedAt) {
    int64_t tid = axon::thread_create(*db, "t");
    int64_t sid = axon::session_start(*db, tid, "s");
    axon::session_end(*db, sid, nullptr, false);

    auto sessions = axon::thread_get_sessions(*db, tid);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_FALSE(sessions[0].ended_at.empty());
}

TEST_F(DialogueTest, SessionStartIsIdempotentWhenKeyIsProvided) {
    int64_t tid = axon::thread_create(*db, "t");
    int64_t first = axon::session_start(*db, tid, "first label", "delivery-42");
    int64_t replay = axon::session_start(*db, tid, "ignored replay label", "delivery-42");
    EXPECT_EQ(first, replay);

    auto sessions = axon::thread_get_sessions(*db, tid);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].label, "first label");
}

TEST_F(DialogueTest, SessionStartWithoutKeyPreservesCreateBehavior) {
    int64_t tid = axon::thread_create(*db, "t");
    int64_t first = axon::session_start(*db, tid, "same label");
    int64_t second = axon::session_start(*db, tid, "same label");
    EXPECT_NE(first, second);
}

// ── Typed handoffs ───────────────────────────────────────────────────────────

TEST_F(DialogueTest, HandoffLifecycleIsTypedAndIdempotent) {
    int64_t thread_id = axon::thread_create(*db, "handoff-thread");
    int64_t session_id = axon::session_start(*db, thread_id, "source");

    int64_t first = axon::handoff_create(*db, session_id, "reviewer", "/repo", "/repo/src",
                                         "Review the queue", "Focus on retries", "review-42");
    int64_t replay = axon::handoff_create(*db, session_id, "reviewer", "/repo", "/repo/src",
                                          "Ignored replay", "", "review-42");
    EXPECT_EQ(first, replay);

    auto pending = axon::handoff_get(*db, first);
    EXPECT_EQ(pending.status, "pending");
    EXPECT_EQ(pending.target_agent, "reviewer");
    EXPECT_EQ(pending.objective, "Review the queue");

    auto claimed = axon::handoff_claim(*db, first, "agent-a");
    EXPECT_EQ(claimed.status, "claimed");
    EXPECT_EQ(claimed.claimed_by, "agent-a");
    EXPECT_EQ(axon::handoff_claim(*db, first, "agent-a").status, "claimed");
    EXPECT_THROW(axon::handoff_claim(*db, first, "agent-b"), std::invalid_argument);
    EXPECT_THROW(axon::handoff_complete(*db, first, "agent-b", "wrong"), std::invalid_argument);

    auto completed = axon::handoff_complete(*db, first, "agent-a", "verified");
    EXPECT_EQ(completed.status, "completed");
    EXPECT_EQ(completed.result, "verified");
    EXPECT_EQ(axon::handoff_complete(*db, first, "agent-a", "ignored").status, "completed");
    EXPECT_THROW(axon::handoff_cancel(*db, first), std::invalid_argument);

    auto listed = axon::handoff_list(*db, "completed", "reviewer");
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0].id, first);
}

TEST_F(DialogueTest, PendingOrClaimedHandoffCanBeCancelled) {
    int64_t first = axon::handoff_create(*db, -1, "worker", "/repo", "/repo", "Do work");
    EXPECT_EQ(axon::handoff_cancel(*db, first).status, "cancelled");
    EXPECT_EQ(axon::handoff_cancel(*db, first).status, "cancelled");

    int64_t second = axon::handoff_create(*db, -1, "worker", "/repo", "/repo", "Do more");
    axon::handoff_claim(*db, second, "agent-a");
    EXPECT_EQ(axon::handoff_cancel(*db, second).status, "cancelled");
}

TEST_F(DialogueTest, HandoffRejectsMissingSourceSession) {
    EXPECT_THROW(axon::handoff_create(*db, 999999, "worker", "/repo", "/repo", "Do work"),
                 std::invalid_argument);
}

// ── Turn CRUD ─────────────────────────────────────────────────────────────────

TEST_F(DialogueTest, TurnAddAndRetrieve) {
    int64_t tid = axon::thread_create(*db, "t");
    int64_t sid = axon::session_start(*db, tid, "s");

    int64_t id1 = axon::turn_add(*db, nullptr, sid, "user", "Hello from user");
    int64_t id2 = axon::turn_add(*db, nullptr, sid, "assistant", "Hello back");
    EXPECT_GT(id1, 0);
    EXPECT_GT(id2, 0);
    EXPECT_NE(id1, id2);

    auto turns = axon::session_get(*db, sid);
    ASSERT_EQ(turns.size(), 2u);
    EXPECT_EQ(turns[0].role, "user");
    EXPECT_EQ(turns[0].content, "Hello from user");
    EXPECT_EQ(turns[1].role, "assistant");
    EXPECT_EQ(turns[1].content, "Hello back");
}

TEST_F(DialogueTest, TurnContentVerbatim) {
    int64_t tid = axon::thread_create(*db, "t");
    int64_t sid = axon::session_start(*db, tid, "s");
    std::string verbatim = "The TTL for auth/token.ts must be 7 days — don't summarize this!";
    axon::turn_add(*db, nullptr, sid, "user", verbatim);
    auto turns = axon::session_get(*db, sid);
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].content, verbatim);
}

// ── Auto-anchor ───────────────────────────────────────────────────────────────

TEST_F(DialogueTest, AutoAnchorNoFilesNoSymbols) {
    int64_t tid = axon::thread_create(*db, "t");
    int64_t sid = axon::session_start(*db, tid, "s");
    int64_t turn_id = axon::turn_add(*db, nullptr, sid, "user", "Just a plain sentence.");
    auto anchors = axon::turn_get_anchors(*db, turn_id);
    EXPECT_EQ(anchors.size(), 0u);
}

TEST_F(DialogueTest, AutoAnchorDetectsFilePath) {
    // Insert a file into the files table so auto_anchor can find it
    db->conn().Query("INSERT INTO files (id, path, language, hash, byte_size) VALUES "
                     "(nextval('seq_id'), 'src/auth/token.ts', 'typescript', 'abc', 100)");

    int64_t tid = axon::thread_create(*db, "t");
    int64_t sid = axon::session_start(*db, tid, "s");
    int64_t tid2 =
        axon::turn_add(*db, nullptr, sid, "user", "The file auth/token.ts needs a TTL of 7 days.");
    auto anchors = axon::turn_get_anchors(*db, tid2);
    EXPECT_GE(anchors.size(), 1u);

    bool found_file_anchor = false;
    for (const auto& a : anchors)
        if (a.file_id > 0) found_file_anchor = true;
    EXPECT_TRUE(found_file_anchor);
}

TEST_F(DialogueTest, AutoAnchorDetectsSymbolName) {
    // Insert a file and a symbol
    db->conn().Query("INSERT INTO files (id, path, language, hash, byte_size) VALUES "
                     "(nextval('seq_id'), 'src/auth.ts', 'typescript', 'abc', 100)");
    auto fres = db->conn().Query("SELECT id FROM files WHERE path = 'src/auth.ts'");
    int64_t fid = fres->GetValue<int64_t>(0, 0);

    db->conn().Query("INSERT INTO symbols (id, file_id, name, kind, start_line, end_line) VALUES "
                     "(nextval('seq_id'), " +
                     std::to_string(fid) + ", 'validateToken', 'function', 10, 30)");
    // Create an edge so the symbol appears in top-500 cache used by auto_anchor
    auto sres = db->conn().Query("SELECT id FROM symbols WHERE name = 'validateToken'");
    int64_t sid_sym = sres->GetValue<int64_t>(0, 0);
    db->conn().Query(
        "INSERT INTO edges (id, from_file, to_file, from_symbol, to_symbol, kind) VALUES "
        "(nextval('seq_id'), " +
        std::to_string(fid) + ", " + std::to_string(fid) + ", " + std::to_string(sid_sym) + ", " +
        std::to_string(sid_sym) + ", 'calls')");

    int64_t tid = axon::thread_create(*db, "t");
    int64_t session_id = axon::session_start(*db, tid, "s");
    int64_t turn_id = axon::turn_add(*db, nullptr, session_id, "user",
                                     "The validateToken function should reject expired JWTs.");
    auto anchors = axon::turn_get_anchors(*db, turn_id);

    bool found_symbol_anchor = false;
    for (const auto& a : anchors)
        if (a.symbol_id > 0) found_symbol_anchor = true;
    EXPECT_TRUE(found_symbol_anchor);
}

// ── Manual anchor ─────────────────────────────────────────────────────────────

TEST_F(DialogueTest, ManualAnchorLink) {
    db->conn().Query("INSERT INTO files (id, path, language, hash, byte_size) VALUES "
                     "(nextval('seq_id'), 'src/main.ts', 'typescript', 'abc', 100)");
    auto fres = db->conn().Query("SELECT id FROM files WHERE path = 'src/main.ts'");
    int64_t fid = fres->GetValue<int64_t>(0, 0);

    int64_t tid = axon::thread_create(*db, "t");
    int64_t sid = axon::session_start(*db, tid, "s");
    int64_t turn_id = axon::turn_add(*db, nullptr, sid, "user", "Some content.");

    int64_t anchor_id = axon::anchor_link(*db, turn_id, fid, -1, "decides");
    EXPECT_GT(anchor_id, 0);

    auto anchors = axon::turn_get_anchors(*db, turn_id);
    bool found = false;
    for (const auto& a : anchors)
        if (a.file_id == fid && a.kind == "decides") found = true;
    EXPECT_TRUE(found);
}

// ── Digest generation ─────────────────────────────────────────────────────────

TEST_F(DialogueTest, SessionEndComputesDigest) {
    int64_t tid = axon::thread_create(*db, "t");
    int64_t sid = axon::session_start(*db, tid, "review");
    axon::turn_add(*db, nullptr, sid, "user", "What's the auth flow?");
    axon::turn_add(*db, nullptr, sid, "assistant", "It uses JWT with 7-day TTL.");
    axon::session_end(*db, sid, nullptr, true);

    auto sessions = axon::thread_get_sessions(*db, tid);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_FALSE(sessions[0].digest.empty());
    EXPECT_NE(sessions[0].digest.find("[SESSION:"), std::string::npos);
}

// ── Symbol name cache ─────────────────────────────────────────────────────────

TEST_F(DialogueTest, LoadSymbolNameCacheEmpty) {
    auto cache = axon::load_symbol_name_cache(*db, 500);
    EXPECT_TRUE(cache.empty());
}

TEST_F(DialogueTest, LoadSymbolNameCacheWithSymbols) {
    db->conn().Query("INSERT INTO files (id, path, language, hash, byte_size) VALUES "
                     "(nextval('seq_id'), 'a.ts', 'typescript', 'x', 10)");
    auto fr = db->conn().Query("SELECT id FROM files WHERE path = 'a.ts'");
    int64_t fid = fr->GetValue<int64_t>(0, 0);

    db->conn().Query("INSERT INTO symbols (id, file_id, name, kind, start_line, end_line) VALUES "
                     "(nextval('seq_id'), " +
                     std::to_string(fid) + ", 'myFunction', 'function', 1, 10)");
    auto sr = db->conn().Query("SELECT id FROM symbols WHERE name = 'myFunction'");
    int64_t sid_sym = sr->GetValue<int64_t>(0, 0);

    db->conn().Query(
        "INSERT INTO edges (id, from_file, to_file, from_symbol, to_symbol, kind) VALUES "
        "(nextval('seq_id'), " +
        std::to_string(fid) + ", " + std::to_string(fid) + ", " + std::to_string(sid_sym) + ", " +
        std::to_string(sid_sym) + ", 'calls')");

    auto cache = axon::load_symbol_name_cache(*db, 500);
    EXPECT_TRUE(cache.count("myFunction") > 0);
}

// ── Pending embed (no model) ──────────────────────────────────────────────────

TEST_F(DialogueTest, EmbedPendingTurnsNoModel_ZeroCount) {
    int64_t tid = axon::thread_create(*db, "t");
    int64_t sid = axon::session_start(*db, tid, "s");
    axon::turn_add(*db, nullptr, sid, "user", "Hello");

    // Verify turns exist with null embedding
    auto res = db->conn().Query("SELECT COUNT(*) FROM turns WHERE embedding IS NULL");
    EXPECT_EQ(res->GetValue<int64_t>(0, 0), 1);

    // Without a model, embed_pending_turns is not called here — just verify table state
    auto turns = axon::session_get(*db, sid);
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].content, "Hello");
}
