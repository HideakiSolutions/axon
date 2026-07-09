#include <gtest/gtest.h>
#include "core/db.hpp"
#include "core/dialogue.hpp"
#include "core/embeddings.hpp"
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

// ── Helpers ───────────────────────────────────────────────────────────────────

static fs::path make_temp_db() {
    static int counter = 0;
    return fs::temp_directory_path() / ("axon_sem_test_" + std::to_string(::getpid()) + "_" +
                                        std::to_string(++counter) + ".duckdb");
}

// ── Fixture: loads the real embedding model, skips when unavailable ───────────

class SemanticTest : public ::testing::Test {
protected:
    fs::path db_path;
    std::unique_ptr<axon::Database> db;
    std::unique_ptr<axon::EmbeddingModel> model;

    void SetUp() override {
        const char* env = std::getenv("AXON_EMBEDDING_MODEL");
        if (!env || std::string(env).empty()) {
            GTEST_SKIP() << "Set AXON_EMBEDDING_MODEL to a GGUF embedding model "
                         << "to enable semantic tests";
        }

        fs::path mp = env;
        if (!fs::exists(mp)) {
            GTEST_SKIP() << "Embedding model not found at " << mp.string()
                         << " — set AXON_EMBEDDING_MODEL to enable semantic tests";
        }
        try {
            model = std::make_unique<axon::EmbeddingModel>(mp);
        } catch (const std::exception& e) {
            GTEST_SKIP() << "Failed to load embedding model: " << e.what();
        }

        db_path = make_temp_db();
        db = std::make_unique<axon::Database>(db_path);
    }

    void TearDown() override {
        model.reset();
        db.reset();
        fs::remove(db_path);
    }

    int64_t make_session(const std::string& thread_name = "t", const std::string& label = "s") {
        int64_t tid = axon::thread_create(*db, thread_name);
        return axon::session_start(*db, tid, label);
    }

    int64_t insert_file(const std::string& path) {
        db->conn().Query("INSERT INTO files (id, path, language, hash, byte_size) VALUES "
                         "(nextval('seq_id'), '" +
                         path + "', 'typescript', 'h1', 500)");
        auto r = db->conn().Query("SELECT id FROM files WHERE path = '" + path + "'");
        return r->GetValue<int64_t>(0, 0);
    }
};

// =============================================================================
// embed_pending_turns — drenagem real
// =============================================================================

TEST_F(SemanticTest, EmbedPendingTurns_FillsEmbedding) {
    int64_t sid = make_session();
    // Add without model → embedding IS NULL
    axon::turn_add(*db, nullptr, sid, "user", "auth token TTL configuration");

    auto before = db->conn().Query("SELECT COUNT(*) FROM turns WHERE embedding IS NULL");
    EXPECT_EQ(before->GetValue<int64_t>(0, 0), 1);

    int drained = axon::embed_pending_turns(*db, *model);
    EXPECT_EQ(drained, 1);

    auto after = db->conn().Query("SELECT COUNT(*) FROM turns WHERE embedding IS NOT NULL");
    EXPECT_EQ(after->GetValue<int64_t>(0, 0), 1);
}

TEST_F(SemanticTest, EmbedPendingTurns_IdempotentWhenAlreadyEmbedded) {
    int64_t sid = make_session();
    // Add WITH model → embedding already filled inline
    axon::turn_add(*db, model.get(), sid, "user", "already embedded turn");

    auto r = db->conn().Query("SELECT COUNT(*) FROM turns WHERE embedding IS NULL");
    EXPECT_EQ(r->GetValue<int64_t>(0, 0), 0);

    int drained = axon::embed_pending_turns(*db, *model);
    EXPECT_EQ(drained, 0); // nothing pending
}

// =============================================================================
// turn_search — busca semântica
// =============================================================================

TEST_F(SemanticTest, TurnSearch_SemanticRelevanceOrdered) {
    int64_t sid = make_session();

    // Three semantically distinct turns
    axon::turn_add(*db, model.get(), sid, "user",
                   "The JWT token validation in auth/token.ts must enforce 7-day TTL.");
    axon::turn_add(*db, model.get(), sid, "user",
                   "The database migration script alters the schema for the users table.");
    axon::turn_add(*db, model.get(), sid, "user",
                   "The CSS button on the homepage needs a larger border-radius.");

    auto hits = axon::turn_search(*db, *model, "JWT authentication token expiry", 3);
    ASSERT_GE(hits.size(), 2u);

    // Top result must be the auth/token turn
    EXPECT_NE(hits[0].turn.content.find("JWT"), std::string::npos);
    // Scores must be in descending order
    EXPECT_GE(hits[0].score, hits[1].score);
}

TEST_F(SemanticTest, TurnSearch_ThreadScoped) {
    // Thread A — auth topic
    int64_t tid_a = axon::thread_create(*db, "thread-auth");
    int64_t sid_a = axon::session_start(*db, tid_a, "");
    axon::turn_add(*db, model.get(), sid_a, "user",
                   "JWT token validation must check expiry before granting access.");

    // Thread B — unrelated topic
    int64_t tid_b = axon::thread_create(*db, "thread-ui");
    int64_t sid_b = axon::session_start(*db, tid_b, "");
    axon::turn_add(*db, model.get(), sid_b, "user",
                   "The dashboard graph component needs a legend toggle button.");

    // Global search returns both
    auto all = axon::turn_search(*db, *model, "token authentication", 10);
    EXPECT_GE(all.size(), 2u);

    // Scoped to thread-auth returns only thread A turns
    auto scoped = axon::turn_search(*db, *model, "token authentication", 10, tid_a);
    ASSERT_GE(scoped.size(), 1u);
    for (const auto& h : scoped)
        EXPECT_EQ(h.thread_name, "thread-auth");
}

TEST_F(SemanticTest, TurnSearch_EmptyWhenNoEmbeddings) {
    int64_t sid = make_session();
    // Add turns WITHOUT model → embedding IS NULL → excluded by WHERE clause
    axon::turn_add(*db, nullptr, sid, "user", "some content about auth tokens");

    auto hits = axon::turn_search(*db, *model, "auth tokens", 5);
    EXPECT_EQ(hits.size(), 0u); // WHERE embedding IS NOT NULL filters them out
}

// =============================================================================
// turns_for_files — busca ancorada + token budget
// =============================================================================

TEST_F(SemanticTest, TurnsForFiles_ReturnsAnchoredTurns) {
    int64_t fid = insert_file("src/auth/token.ts");
    int64_t sid = make_session();

    // Add turn that auto-anchors to token.ts
    axon::turn_add(*db, model.get(), sid, "user",
                   "The file auth/token.ts must validate JWT expiry strictly.");

    auto hits = axon::turns_for_files(*db, *model, "auth token", {fid}, 5000);
    ASSERT_GE(hits.size(), 1u);
    EXPECT_NE(hits[0].turn.content.find("token.ts"), std::string::npos);
}

TEST_F(SemanticTest, TurnsForFiles_RespectsTokenBudget) {
    int64_t fid = insert_file("src/core/db.cpp");
    int64_t sid = make_session();

    // Each turn: ~200 chars ≈ 50 tokens
    std::string body(200, 'x');
    for (int i = 0; i < 5; i++) {
        int64_t tid =
            axon::turn_add(*db, model.get(), sid, "user", "Refactor db.cpp query: " + body);
        axon::anchor_link(*db, tid, fid, -1, "mentions");
    }

    // Budget = 75 tokens → fits at most 1 turn of ~50 tokens
    auto hits = axon::turns_for_files(*db, *model, "database query", {fid}, 75);
    EXPECT_LE(hits.size(), 2u); // at most 1-2 turns within 75-token budget
}

TEST_F(SemanticTest, TurnsForFiles_EmptyWhenNoAnchors) {
    int64_t fid = insert_file("src/orphan.ts");
    int64_t sid = make_session();
    axon::turn_add(*db, model.get(), sid, "user", "nothing about orphan here");

    auto hits = axon::turns_for_files(*db, *model, "orphan file", {fid}, 5000);
    EXPECT_EQ(hits.size(), 0u);
}

// =============================================================================
// session_end com modelo — digest_embedding preenchido
// =============================================================================

TEST_F(SemanticTest, SessionEnd_DigestEmbeddingPopulated) {
    int64_t tid = axon::thread_create(*db, "auth-sprint");
    int64_t sid = axon::session_start(*db, tid, "Token TTL review");

    axon::turn_add(*db, model.get(), sid, "user", "What is the token TTL?");
    axon::turn_add(*db, model.get(), sid, "assistant", "7 days — set in validateToken.");

    axon::session_end(*db, sid, model.get(), true);

    auto r = db->conn().Query("SELECT digest IS NOT NULL, digest_embedding IS NOT NULL"
                              " FROM sessions WHERE id = " +
                              std::to_string(sid));
    ASSERT_EQ(r->RowCount(), 1u);
    EXPECT_TRUE(r->GetValue<bool>(0, 0)) << "digest should be non-null";
    EXPECT_TRUE(r->GetValue<bool>(1, 0)) << "digest_embedding should be non-null";
}
