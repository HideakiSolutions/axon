#pragma once
#include "db.hpp"
#include "embeddings.hpp"
#include <string>
#include <vector>
#include <unordered_set>

namespace axon {

// ── Data structs ──────────────────────────────────────────────────────────────

struct Thread {
    int64_t id;
    std::string name;
    std::string kind; // project | person | topic
    std::string created_at;
};

struct Session {
    int64_t id;
    int64_t thread_id;
    std::string label;
    std::string started_at;
    std::string ended_at;
    std::string digest;
};

struct Turn {
    int64_t id;
    int64_t session_id;
    std::string role; // user | assistant
    std::string content;
    std::string ts;
};

struct Anchor {
    int64_t id;
    int64_t turn_id;
    int64_t file_id;
    int64_t symbol_id;
    std::string kind; // mentions | decides | questions
};

struct TurnHit {
    Turn turn;
    float score;
    std::string session_label;
    std::string thread_name;
};

struct Handoff {
    int64_t id;
    int64_t source_session_id;
    std::string target_agent;
    std::string project_root;
    std::string working_directory;
    std::string objective;
    std::string context;
    std::string status;
    std::string claimed_by;
    std::string result;
    std::string idempotency_key;
    std::string created_at;
    std::string claimed_at;
    std::string completed_at;
};

// ── Thread operations ─────────────────────────────────────────────────────────

int64_t thread_create(Database& db, const std::string& name, const std::string& kind = "project");
std::vector<Thread> thread_list(Database& db);

// ── Session operations ────────────────────────────────────────────────────────

int64_t session_start(Database& db, int64_t thread_id, const std::string& label = "",
                      const std::string& idempotency_key = "");
void session_end(Database& db, int64_t session_id,
                 EmbeddingModel* model, // nullable
                 bool compute_digest = true);
std::vector<Turn> session_get(Database& db, int64_t session_id, int limit = 500);
std::vector<Session> thread_get_sessions(Database& db, int64_t thread_id);

// ── Typed handoff operations ─────────────────────────────────────────────────

int64_t handoff_create(Database& db, int64_t source_session_id, const std::string& target_agent,
                       const std::string& project_root, const std::string& working_directory,
                       const std::string& objective, const std::string& context = "",
                       const std::string& idempotency_key = "");
Handoff handoff_get(Database& db, int64_t handoff_id);
std::vector<Handoff> handoff_list(Database& db, const std::string& status = "",
                                  const std::string& target_agent = "", int limit = 100);
Handoff handoff_claim(Database& db, int64_t handoff_id, const std::string& claimed_by);
Handoff handoff_complete(Database& db, int64_t handoff_id, const std::string& claimed_by,
                         const std::string& result = "");
Handoff handoff_cancel(Database& db, int64_t handoff_id);

// ── Turn operations ───────────────────────────────────────────────────────────

int64_t turn_add(Database& db, EmbeddingModel* model, int64_t session_id, const std::string& role,
                 const std::string& content);

// Detect file paths and symbol names in content, insert matching turn_anchors.
// Uses the project's files/symbols tables — no external NLP needed.
void auto_anchor(Database& db, int64_t turn_id, const std::string& content);

// Manual anchor: link a turn to a specific file or symbol.
int64_t anchor_link(Database& db, int64_t turn_id, int64_t file_id, int64_t symbol_id,
                    const std::string& kind = "mentions");

std::vector<Anchor> turn_get_anchors(Database& db, int64_t turn_id);

// ── Semantic search ───────────────────────────────────────────────────────────

// Semantic search over all turns. Pass thread_id=-1 for global scope.
std::vector<TurnHit> turn_search(Database& db, EmbeddingModel& model, const std::string& query,
                                 int limit = 5, int64_t thread_id = -1);

// Turns anchored to the given file_ids + semantically similar to query.
// Used by assemble_capsule when dialogue_budget > 0.
std::vector<TurnHit> turns_for_files(Database& db, EmbeddingModel& model, const std::string& query,
                                     const std::vector<int64_t>& file_ids, int token_budget = 1000);

// ── Embed pipeline ────────────────────────────────────────────────────────────

// Embed all turns WHERE embedding IS NULL. Mirrors embed_pending_symbols().
int embed_pending_turns(Database& db, EmbeddingModel& model, int limit = 5000);

// ── Symbol name cache ─────────────────────────────────────────────────────────

// Returns the top-N most-referenced symbol names in the graph.
// Used by auto_anchor() to avoid a full-table scan per word.
std::unordered_set<std::string> load_symbol_name_cache(Database& db, int limit = 500);

} // namespace axon
