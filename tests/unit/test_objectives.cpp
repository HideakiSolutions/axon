#include <gtest/gtest.h>
#include "core/db.hpp"
#include "core/dialogue.hpp"
#include "core/capsule.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ── Helpers ───────────────────────────────────────────────────────────────────

static fs::path make_temp_db() {
    static int counter = 0;
    return fs::temp_directory_path() /
           ("axon_obj_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(++counter) + ".duckdb");
}

class ObjTest : public ::testing::Test {
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

    int64_t file_id(const std::string& path) {
        db->conn().Query(
            "INSERT INTO files (id, path, language, hash, byte_size) VALUES "
            "(nextval('seq_id'), '" + path + "', 'typescript', 'hash1', 500)");
        auto r = db->conn().Query("SELECT id FROM files WHERE path = '" + path + "'");
        return r->GetValue<int64_t>(0, 0);
    }

    int64_t make_thread_session() {
        int64_t tid = axon::thread_create(*db, "t");
        return axon::session_start(*db, tid, "s");
    }
};

// =============================================================================
// OBJETIVO 1 — TOKEN REDUCTION
// =============================================================================

// ── estimate_tokens: fórmula chars/4 (teto inteiro) ─────────────────────────

TEST(TokenEstimate, EmptyString) {
    EXPECT_EQ(axon::estimate_tokens(""), 0);
}

TEST(TokenEstimate, FourCharsIsOneToken) {
    EXPECT_EQ(axon::estimate_tokens("abcd"), 1);
}

TEST(TokenEstimate, FiveCharsIsTwoTokens) {
    EXPECT_EQ(axon::estimate_tokens("abcde"), 2);  // (5+3)/4 = 2
}

TEST(TokenEstimate, Monotonic) {
    std::string s;
    int prev = 0;
    for (int i = 0; i < 200; i++) {
        s += 'x';
        int cur = axon::estimate_tokens(s);
        EXPECT_GE(cur, prev);
        prev = cur;
    }
}

TEST(TokenEstimate, LargeContent) {
    std::string big(4000, 'a');
    EXPECT_EQ(axon::estimate_tokens(big), 1000);
}

// ── Token budget: dialogue turns respeitam o budget via token_estimate ────────

TEST_F(ObjTest, DialogueTurnTokenEstimateAccumulates) {
    // Simula o que assemble_capsule faz ao popular related_turns:
    // token_estimate de cada turn é somado ao total da cápsula.
    int64_t sid = make_thread_session();
    std::string content_a(400, 'a');  // 100 tokens
    std::string content_b(400, 'b');  // 100 tokens

    axon::turn_add(*db, nullptr, sid, "user",      content_a);
    axon::turn_add(*db, nullptr, sid, "assistant", content_b);

    auto turns = axon::session_get(*db, sid);
    ASSERT_EQ(turns.size(), 2u);

    int total_tokens = 0;
    for (const auto& t : turns)
        total_tokens += axon::estimate_tokens(t.content);

    EXPECT_EQ(total_tokens, 200);  // 100 + 100
}

TEST_F(ObjTest, TokenBudgetDoesNotExceedForSmallContent) {
    // Verifica que estimate_tokens(content) <= content.size() sempre.
    // (A fórmula ceil(n/4) < n para n > 1, portanto cápsula < raw bytes.)
    int64_t sid = make_thread_session();
    std::string large_content(8000, 'z');  // 8 kB raw

    axon::turn_add(*db, nullptr, sid, "user", large_content);
    auto turns = axon::session_get(*db, sid);
    ASSERT_EQ(turns.size(), 1u);

    int tokens = axon::estimate_tokens(turns[0].content);
    EXPECT_EQ(tokens, 2000);                         // 8000/4
    EXPECT_LT(tokens, (int)large_content.size());    // tokens < raw bytes
}

// ── Pending embed tracking: turns sem modelo → embedding IS NULL ──────────────

TEST_F(ObjTest, TurnsAddedWithoutModelHaveNullEmbedding) {
    int64_t sid = make_thread_session();
    axon::turn_add(*db, nullptr, sid, "user", "sem modelo");
    axon::turn_add(*db, nullptr, sid, "user", "outro turn");

    auto r = db->conn().Query("SELECT COUNT(*) FROM turns WHERE embedding IS NULL");
    EXPECT_EQ(r->GetValue<int64_t>(0, 0), 2);
}

TEST_F(ObjTest, EmbedPendingCountZeroWhenNoNullEmbeddings) {
    // Sem turns, não há nada para drená — a função retornaria 0.
    // Verificamos via SQL (sem modelo disponível no ambiente de CI).
    int64_t sid = make_thread_session();
    auto r = db->conn().Query("SELECT COUNT(*) FROM turns WHERE embedding IS NULL");
    EXPECT_EQ(r->GetValue<int64_t>(0, 0), 0);
}

// =============================================================================
// OBJETIVO 2 — MEMÓRIA ESTRUTURADA (paridade de capacidades)
// =============================================================================

// ── Verbatim: caracteres especiais preservados (SQL injection chars) ──────────

TEST_F(ObjTest, VerbatimSingleQuotesPreserved) {
    int64_t sid = make_thread_session();
    std::string tricky = "Don't change auth/token.ts — it's the owner's token!";
    axon::turn_add(*db, nullptr, sid, "user", tricky);
    auto turns = axon::session_get(*db, sid);
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].content, tricky);
}

TEST_F(ObjTest, VerbatimBackslashesAndNewlines) {
    int64_t sid = make_thread_session();
    std::string tricky = "path\\to\\file\nline2\ttabbed";
    axon::turn_add(*db, nullptr, sid, "assistant", tricky);
    auto turns = axon::session_get(*db, sid);
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].content, tricky);
}

// ── Isolamento de sessões: turns de sessões distintas não vazam ───────────────

TEST_F(ObjTest, SessionIsolation) {
    int64_t tid  = axon::thread_create(*db, "t");
    int64_t sid1 = axon::session_start(*db, tid, "session-1");
    int64_t sid2 = axon::session_start(*db, tid, "session-2");

    axon::turn_add(*db, nullptr, sid1, "user", "turn in session 1");
    axon::turn_add(*db, nullptr, sid2, "user", "turn in session 2");

    auto t1 = axon::session_get(*db, sid1);
    auto t2 = axon::session_get(*db, sid2);

    ASSERT_EQ(t1.size(), 1u);
    ASSERT_EQ(t2.size(), 1u);
    EXPECT_EQ(t1[0].content, "turn in session 1");
    EXPECT_EQ(t2[0].content, "turn in session 2");
}

// ── Isolamento de threads ─────────────────────────────────────────────────────

TEST_F(ObjTest, ThreadIsolation) {
    int64_t tA = axon::thread_create(*db, "thread-A");
    int64_t tB = axon::thread_create(*db, "thread-B");
    int64_t sA = axon::session_start(*db, tA, "");
    int64_t sB = axon::session_start(*db, tB, "");

    axon::turn_add(*db, nullptr, sA, "user", "only in A");
    axon::turn_add(*db, nullptr, sB, "user", "only in B");

    auto sessA = axon::thread_get_sessions(*db, tA);
    auto sessB = axon::thread_get_sessions(*db, tB);
    ASSERT_EQ(sessA.size(), 1u);
    ASSERT_EQ(sessB.size(), 1u);
    EXPECT_EQ(sessA[0].thread_id, tA);
    EXPECT_EQ(sessB[0].thread_id, tB);
}

// ── Anchor deduplication: mesmo arquivo mencionado 2x → 1 anchor ─────────────

TEST_F(ObjTest, AnchorDeduplicationSameFileTwice) {
    int64_t fid = file_id("src/auth/token.ts");
    int64_t sid = make_thread_session();

    // Content mentions "token.ts" twice
    int64_t turn_id = axon::turn_add(*db, nullptr, sid, "user",
        "Change token.ts TTL. Also check token.ts imports.");

    auto anchors = axon::turn_get_anchors(*db, turn_id);
    int file_anchors = 0;
    for (const auto& a : anchors)
        if (a.file_id == fid) file_anchors++;
    EXPECT_EQ(file_anchors, 1);  // deduplicado
}

// ── Anchor: múltiplos arquivos distintos em um turn ───────────────────────────

TEST_F(ObjTest, AnchorMultipleDistinctFiles) {
    int64_t fa = file_id("src/auth/token.ts");
    int64_t fb = file_id("src/core/db.cpp");
    int64_t sid = make_thread_session();

    int64_t turn_id = axon::turn_add(*db, nullptr, sid, "user",
        "Update token.ts and also refactor db.cpp query paths.");

    auto anchors = axon::turn_get_anchors(*db, turn_id);
    std::set<int64_t> found_files;
    for (const auto& a : anchors)
        if (a.file_id > 0) found_files.insert(a.file_id);

    EXPECT_TRUE(found_files.count(fa) > 0);
    EXPECT_TRUE(found_files.count(fb) > 0);
}

// ── Digest ADF: estrutura obrigatória presente ────────────────────────────────

TEST_F(ObjTest, DigestADFHasRequiredMarkers) {
    int64_t tid = axon::thread_create(*db, "auth-review");
    int64_t sid = axon::session_start(*db, tid, "Sprint TTL");

    axon::turn_add(*db, nullptr, sid, "user",      "What's the auth token TTL?");
    axon::turn_add(*db, nullptr, sid, "assistant", "It is 7 days, set in validateToken.");
    axon::session_end(*db, sid, nullptr, true);

    auto sessions = axon::thread_get_sessions(*db, tid);
    ASSERT_EQ(sessions.size(), 1u);
    const std::string& d = sessions[0].digest;

    EXPECT_FALSE(d.empty());
    EXPECT_NE(d.find("[SESSION:"),  std::string::npos) << "ADF missing [SESSION:] marker";
    EXPECT_NE(d.find("Sprint TTL"), std::string::npos) << "ADF missing session label";
    EXPECT_NE(d.find("---"),        std::string::npos) << "ADF missing turn separator";
    EXPECT_NE(d.find("user:"),      std::string::npos) << "ADF missing user role";
    EXPECT_NE(d.find("assistant:"), std::string::npos) << "ADF missing assistant role";
}

TEST_F(ObjTest, DigestADFWithAnchorsListsFiles) {
    int64_t fid = file_id("src/auth/token.ts");
    int64_t tid = axon::thread_create(*db, "t");
    int64_t sid = axon::session_start(*db, tid, "anchor-session");

    axon::turn_add(*db, nullptr, sid, "user",
                   "The file auth/token.ts has a 7-day TTL.");
    axon::session_end(*db, sid, nullptr, true);

    auto sessions = axon::thread_get_sessions(*db, tid);
    ASSERT_EQ(sessions.size(), 1u);
    // [ANCHORS:] line should mention the detected file
    EXPECT_NE(sessions[0].digest.find("[ANCHORS:"), std::string::npos);
    EXPECT_NE(sessions[0].digest.find("token.ts"),  std::string::npos);
}

// ── Digest: sessão vazia produz ADF válido ────────────────────────────────────

TEST_F(ObjTest, DigestEmptySessionIsValidADF) {
    int64_t tid = axon::thread_create(*db, "t");
    int64_t sid = axon::session_start(*db, tid, "empty");
    axon::session_end(*db, sid, nullptr, true);

    auto sessions = axon::thread_get_sessions(*db, tid);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_NE(sessions[0].digest.find("[SESSION:"), std::string::npos);
    EXPECT_NE(sessions[0].digest.find("empty"),     std::string::npos);
}

// ── Thread kinds preservados exatamente ──────────────────────────────────────

TEST_F(ObjTest, ThreadKindsRoundtrip) {
    for (const auto& kind : {"project", "person", "topic"}) {
        axon::thread_create(*db, std::string("t-") + kind, kind);
    }
    auto threads = axon::thread_list(*db);
    ASSERT_EQ(threads.size(), 3u);

    std::set<std::string> kinds;
    for (const auto& t : threads) kinds.insert(t.kind);
    EXPECT_TRUE(kinds.count("project"));
    EXPECT_TRUE(kinds.count("person"));
    EXPECT_TRUE(kinds.count("topic"));
}

// ── Ordem cronológica dos turns garantida ─────────────────────────────────────

TEST_F(ObjTest, TurnsReturnedInChronologicalOrder) {
    int64_t sid = make_thread_session();
    for (int i = 0; i < 5; i++)
        axon::turn_add(*db, nullptr, sid, "user", "msg " + std::to_string(i));

    auto turns = axon::session_get(*db, sid);
    ASSERT_EQ(turns.size(), 5u);
    for (int i = 0; i < 5; i++)
        EXPECT_EQ(turns[i].content, "msg " + std::to_string(i));
}

// ── Anchor manual: kind "decides" e "questions" preservados ──────────────────

TEST_F(ObjTest, ManualAnchorKindRoundtrip) {
    int64_t fid     = file_id("src/main.ts");
    int64_t sid     = make_thread_session();
    int64_t turn_id = axon::turn_add(*db, nullptr, sid, "user", "context");

    axon::anchor_link(*db, turn_id, fid, -1, "decides");
    axon::anchor_link(*db, turn_id, fid, -1, "questions");

    auto anchors = axon::turn_get_anchors(*db, turn_id);
    std::set<std::string> kinds;
    for (const auto& a : anchors)
        if (a.file_id == fid) kinds.insert(a.kind);

    EXPECT_TRUE(kinds.count("decides"));
    EXPECT_TRUE(kinds.count("questions"));
}

// ── Turns append-only: conteúdo não muda após inserção ───────────────────────

TEST_F(ObjTest, TurnsAreAppendOnly) {
    int64_t sid = make_thread_session();
    int64_t tid = axon::turn_add(*db, nullptr, sid, "user", "original content");

    // Tentativa de UPDATE direta não deve ser bloqueada, mas a API não expõe UPDATE
    // — verificamos apenas que session_get retorna o conteúdo original
    auto turns = axon::session_get(*db, sid);
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].content, "original content");
    EXPECT_EQ(turns[0].id, tid);
}

// ── Múltiplos turns: limite respeitado em session_get ────────────────────────

TEST_F(ObjTest, SessionGetLimitRespected) {
    int64_t sid = make_thread_session();
    for (int i = 0; i < 20; i++)
        axon::turn_add(*db, nullptr, sid, "user", "turn " + std::to_string(i));

    auto all    = axon::session_get(*db, sid, 500);
    auto capped = axon::session_get(*db, sid, 5);
    EXPECT_EQ(all.size(),    20u);
    EXPECT_EQ(capped.size(),  5u);
}
