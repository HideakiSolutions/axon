#include "dialogue.hpp"
#include <regex>
#include <sstream>
#include <algorithm>
#include <iostream>

namespace axon {

// ── SQL helpers ───────────────────────────────────────────────────────────────

static std::string sq(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out;
}

static std::string vec_literal(const std::vector<float>& v, int dims) {
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < v.size(); i++) {
        if (i) ss << ",";
        ss << v[i];
    }
    ss << "]::FLOAT[" << dims << "]";
    return ss.str();
}

// ── Thread operations ─────────────────────────────────────────────────────────

int64_t thread_create(Database& db, const std::string& name, const std::string& kind) {
    auto res = db.conn().Query("SELECT id FROM threads WHERE name = '" + sq(name) + "'");
    if (!res->HasError() && res->RowCount() > 0) return res->GetValue<int64_t>(0, 0);

    auto ins = db.conn().Query("INSERT INTO threads (id, name, kind) VALUES ("
                               "nextval('seq_id'), '" +
                               sq(name) + "', '" + sq(kind) + "') RETURNING id");
    if (ins->HasError()) throw std::runtime_error("thread_create: " + ins->GetError());
    return ins->GetValue<int64_t>(0, 0);
}

std::vector<Thread> thread_list(Database& db) {
    auto res = db.conn().Query("SELECT t.id, t.name, t.kind, CAST(t.created_at AS VARCHAR),"
                               "       COUNT(s.id) AS session_count"
                               " FROM threads t"
                               " LEFT JOIN sessions s ON s.thread_id = t.id"
                               " GROUP BY t.id, t.name, t.kind, t.created_at"
                               " ORDER BY t.created_at DESC");
    if (res->HasError()) throw std::runtime_error("thread_list: " + res->GetError());

    std::vector<Thread> out;
    for (duckdb::idx_t i = 0; i < res->RowCount(); i++) {
        Thread t;
        t.id = res->GetValue<int64_t>(0, i);
        t.name = res->GetValue(1, i).ToString();
        t.kind = res->GetValue(2, i).ToString();
        t.created_at = res->GetValue(3, i).ToString();
        out.push_back(t);
    }
    return out;
}

// ── Session operations ────────────────────────────────────────────────────────

int64_t session_start(Database& db, int64_t thread_id, const std::string& label,
                      const std::string& idempotency_key) {
    if (!idempotency_key.empty()) {
        auto existing = db.conn().Query(
            "SELECT id FROM sessions WHERE thread_id = " + std::to_string(thread_id) +
            " AND idempotency_key = '" + sq(idempotency_key) + "' LIMIT 1");
        if (!existing->HasError() && existing->RowCount() > 0)
            return existing->GetValue<int64_t>(0, 0);
    }

    const std::string key_value =
        idempotency_key.empty() ? "NULL" : "'" + sq(idempotency_key) + "'";
    auto ins = db.conn().Query(
        "INSERT INTO sessions (id, thread_id, label, idempotency_key) VALUES ("
        "nextval('seq_id'), " +
        std::to_string(thread_id) + ", '" + sq(label) + "', " + key_value + ") RETURNING id");
    if (ins->HasError()) throw std::runtime_error("session_start: " + ins->GetError());
    return ins->GetValue<int64_t>(0, 0);
}

static std::string build_digest(Database& db, int64_t session_id) {
    // Fetch session info
    auto si = db.conn().Query("SELECT label, CAST(started_at AS VARCHAR), CAST(ended_at AS VARCHAR)"
                              " FROM sessions WHERE id = " +
                              std::to_string(session_id));
    std::string label =
        (!si->HasError() && si->RowCount() > 0) ? si->GetValue(0, 0).ToString() : "";
    std::string started_at =
        (!si->HasError() && si->RowCount() > 0) ? si->GetValue(1, 0).ToString() : "";
    std::string ended_at =
        (!si->HasError() && si->RowCount() > 0) ? si->GetValue(2, 0).ToString() : "";

    // Collect anchored file/symbol names
    auto an =
        db.conn().Query("SELECT DISTINCT COALESCE(f.path, '') AS fp, COALESCE(sym.name, '') AS sn"
                        " FROM turn_anchors ta"
                        " JOIN turns t ON t.id = ta.turn_id"
                        " LEFT JOIN files f ON f.id = ta.file_id"
                        " LEFT JOIN symbols sym ON sym.id = ta.symbol_id"
                        " WHERE t.session_id = " +
                        std::to_string(session_id) + " LIMIT 20");

    std::string anchors_line;
    if (!an->HasError()) {
        std::vector<std::string> parts;
        for (duckdb::idx_t i = 0; i < an->RowCount(); i++) {
            std::string fp = an->GetValue(0, i).ToString();
            std::string sn = an->GetValue(1, i).ToString();
            if (!fp.empty()) {
                // Extract basename for brevity
                auto pos = fp.rfind('/');
                parts.push_back(pos == std::string::npos ? fp : fp.substr(pos + 1));
            }
            if (!sn.empty()) parts.push_back(sn);
        }
        if (!parts.empty()) {
            anchors_line = "[ANCHORS:";
            for (size_t i = 0; i < parts.size(); i++) {
                anchors_line += (i ? ", " : " ") + parts[i];
            }
            anchors_line += "]";
        }
    }

    // Select key turns: anchored ones + first and last
    auto tr = db.conn().Query("SELECT t.id, t.role, t.content"
                              " FROM turns t"
                              " WHERE t.session_id = " +
                              std::to_string(session_id) + " ORDER BY t.ts ASC, t.id ASC");
    if (tr->HasError() || tr->RowCount() == 0) {
        return "[SESSION: " + label + " | " + started_at + " → " + ended_at + "]\n" + anchors_line +
               "\n(no turns)";
    }

    // Collect anchored turn ids
    auto aid = db.conn().Query("SELECT DISTINCT ta.turn_id FROM turn_anchors ta"
                               " JOIN turns t ON t.id = ta.turn_id"
                               " WHERE t.session_id = " +
                               std::to_string(session_id));
    std::unordered_set<int64_t> anchored;
    if (!aid->HasError())
        for (duckdb::idx_t i = 0; i < aid->RowCount(); i++)
            anchored.insert(aid->GetValue<int64_t>(0, i));

    // Build turn list: first, last, and anchored (up to 12 total)
    std::vector<std::pair<std::string, std::string>> selected; // {role, content_trunc}
    int64_t first_id = tr->GetValue<int64_t>(0, 0);
    int64_t last_id = tr->GetValue<int64_t>(0, tr->RowCount() - 1);

    auto add_turn = [&](duckdb::idx_t i) {
        std::string role = tr->GetValue(1, i).ToString();
        std::string content = tr->GetValue(2, i).ToString();
        if (content.size() > 200) content = content.substr(0, 197) + "...";
        selected.push_back({role, content});
    };

    std::unordered_set<int64_t> added;
    // Always include first
    add_turn(0);
    added.insert(first_id);

    // Anchored turns (up to 10 more)
    for (duckdb::idx_t i = 1; i < tr->RowCount() && (int)selected.size() < 11; i++) {
        int64_t tid = tr->GetValue<int64_t>(0, i);
        if (anchored.count(tid) && !added.count(tid)) {
            add_turn(i);
            added.insert(tid);
        }
    }

    // Always include last (if not already)
    if (!added.count(last_id)) {
        add_turn(tr->RowCount() - 1);
        added.insert(last_id);
    }

    std::string body;
    for (const auto& [role, content] : selected) {
        body += role + ": " + content + "\n";
    }

    return "[SESSION: " + label + " | " + started_at + " \xe2\x86\x92 " + ended_at + "]\n" +
           (anchors_line.empty() ? "" : anchors_line + "\n") + "---\n" + body;
}

void session_end(Database& db, int64_t session_id, EmbeddingModel* model, bool compute_digest) {
    db.conn().Query("UPDATE sessions SET ended_at = now() WHERE id = " +
                    std::to_string(session_id));

    if (!compute_digest) return;

    std::string digest = build_digest(db, session_id);

    if (model) {
        auto emb = model->embed(digest);
        db.conn().Query("UPDATE sessions SET digest = '" + sq(digest) +
                        "', digest_embedding = " + vec_literal(emb, model->dims()) +
                        " WHERE id = " + std::to_string(session_id));
    } else {
        db.conn().Query("UPDATE sessions SET digest = '" + sq(digest) +
                        "' WHERE id = " + std::to_string(session_id));
    }
}

std::vector<Turn> session_get(Database& db, int64_t session_id, int limit) {
    auto res = db.conn().Query("SELECT id, session_id, role, content, CAST(ts AS VARCHAR)"
                               " FROM turns WHERE session_id = " +
                               std::to_string(session_id) + " ORDER BY ts ASC, id ASC LIMIT " +
                               std::to_string(limit));
    if (res->HasError()) throw std::runtime_error("session_get: " + res->GetError());

    std::vector<Turn> out;
    for (duckdb::idx_t i = 0; i < res->RowCount(); i++) {
        Turn t;
        t.id = res->GetValue<int64_t>(0, i);
        t.session_id = res->GetValue<int64_t>(1, i);
        t.role = res->GetValue(2, i).ToString();
        t.content = res->GetValue(3, i).ToString();
        t.ts = res->GetValue(4, i).ToString();
        out.push_back(t);
    }
    return out;
}

std::vector<Session> thread_get_sessions(Database& db, int64_t thread_id) {
    auto res =
        db.conn().Query("SELECT id, thread_id, COALESCE(label,''), CAST(started_at AS VARCHAR),"
                        "       COALESCE(CAST(ended_at AS VARCHAR),''), COALESCE(digest,'')"
                        " FROM sessions WHERE thread_id = " +
                        std::to_string(thread_id) + " ORDER BY started_at DESC");
    if (res->HasError()) throw std::runtime_error("thread_get_sessions: " + res->GetError());

    std::vector<Session> out;
    for (duckdb::idx_t i = 0; i < res->RowCount(); i++) {
        Session s;
        s.id = res->GetValue<int64_t>(0, i);
        s.thread_id = res->GetValue<int64_t>(1, i);
        s.label = res->GetValue(2, i).ToString();
        s.started_at = res->GetValue(3, i).ToString();
        s.ended_at = res->GetValue(4, i).ToString();
        s.digest = res->GetValue(5, i).ToString();
        out.push_back(s);
    }
    return out;
}

// ── Typed handoff operations ─────────────────────────────────────────────────

static Handoff handoff_from_row(duckdb::MaterializedQueryResult& result, duckdb::idx_t row) {
    Handoff handoff;
    handoff.id = result.GetValue<int64_t>(0, row);
    handoff.source_session_id = result.GetValue<int64_t>(1, row);
    handoff.target_agent = result.GetValue(2, row).ToString();
    handoff.project_root = result.GetValue(3, row).ToString();
    handoff.working_directory = result.GetValue(4, row).ToString();
    handoff.objective = result.GetValue(5, row).ToString();
    handoff.context = result.GetValue(6, row).ToString();
    handoff.status = result.GetValue(7, row).ToString();
    handoff.claimed_by = result.GetValue(8, row).ToString();
    handoff.result = result.GetValue(9, row).ToString();
    handoff.idempotency_key = result.GetValue(10, row).ToString();
    handoff.created_at = result.GetValue(11, row).ToString();
    handoff.claimed_at = result.GetValue(12, row).ToString();
    handoff.completed_at = result.GetValue(13, row).ToString();
    return handoff;
}

static const char* handoff_projection() {
    return "id, COALESCE(source_session_id, -1), target_agent, project_root, "
           "working_directory, objective, context, status, COALESCE(claimed_by, ''), "
           "COALESCE(result, ''), COALESCE(idempotency_key, ''), "
           "CAST(created_at AS VARCHAR), COALESCE(CAST(claimed_at AS VARCHAR), ''), "
           "COALESCE(CAST(completed_at AS VARCHAR), '')";
}

int64_t handoff_create(Database& db, int64_t source_session_id, const std::string& target_agent,
                       const std::string& project_root, const std::string& working_directory,
                       const std::string& objective, const std::string& context,
                       const std::string& idempotency_key) {
    if (target_agent.empty()) throw std::invalid_argument("target_agent is required");
    if (project_root.empty()) throw std::invalid_argument("project_root is required");
    if (working_directory.empty()) throw std::invalid_argument("working_directory is required");
    if (objective.empty()) throw std::invalid_argument("objective is required");

    if (source_session_id >= 0) {
        auto session = db.conn().Query("SELECT id FROM sessions WHERE id = " +
                                       std::to_string(source_session_id));
        if (session->HasError() || session->RowCount() == 0)
            throw std::invalid_argument("source_session_id does not exist");
    }

    const std::string source_filter =
        source_session_id >= 0 ? "source_session_id = " + std::to_string(source_session_id)
                               : "source_session_id IS NULL";
    if (!idempotency_key.empty()) {
        auto existing =
            db.conn().Query("SELECT id FROM handoffs WHERE " + source_filter +
                            " AND idempotency_key = '" + sq(idempotency_key) + "' LIMIT 1");
        if (!existing->HasError() && existing->RowCount() > 0)
            return existing->GetValue<int64_t>(0, 0);
    }

    const std::string source_value =
        source_session_id >= 0 ? std::to_string(source_session_id) : "NULL";
    const std::string key_value =
        idempotency_key.empty() ? "NULL" : "'" + sq(idempotency_key) + "'";
    auto inserted = db.conn().Query(
        "INSERT INTO handoffs (id, source_session_id, target_agent, project_root, "
        "working_directory, objective, context, idempotency_key) VALUES (nextval('seq_id'), " +
        source_value + ", '" + sq(target_agent) + "', '" + sq(project_root) + "', '" +
        sq(working_directory) + "', '" + sq(objective) + "', '" + sq(context) + "', " + key_value +
        ") RETURNING id");
    if (inserted->HasError()) throw std::runtime_error("handoff_create: " + inserted->GetError());
    return inserted->GetValue<int64_t>(0, 0);
}

Handoff handoff_get(Database& db, int64_t handoff_id) {
    auto result = db.conn().Query("SELECT " + std::string(handoff_projection()) +
                                  " FROM handoffs WHERE id = " + std::to_string(handoff_id));
    if (result->HasError()) throw std::runtime_error("handoff_get: " + result->GetError());
    if (result->RowCount() == 0) throw std::invalid_argument("handoff_id does not exist");
    return handoff_from_row(*result, 0);
}

std::vector<Handoff> handoff_list(Database& db, const std::string& status,
                                  const std::string& target_agent, int limit) {
    if (limit < 1) limit = 1;
    if (limit > 500) limit = 500;
    std::string where = " WHERE 1=1";
    if (!status.empty()) where += " AND status = '" + sq(status) + "'";
    if (!target_agent.empty()) where += " AND target_agent = '" + sq(target_agent) + "'";

    auto result =
        db.conn().Query("SELECT " + std::string(handoff_projection()) + " FROM handoffs" + where +
                        " ORDER BY created_at ASC, id ASC LIMIT " + std::to_string(limit));
    if (result->HasError()) throw std::runtime_error("handoff_list: " + result->GetError());

    std::vector<Handoff> handoffs;
    handoffs.reserve(result->RowCount());
    for (duckdb::idx_t row = 0; row < result->RowCount(); ++row)
        handoffs.push_back(handoff_from_row(*result, row));
    return handoffs;
}

Handoff handoff_claim(Database& db, int64_t handoff_id, const std::string& claimed_by) {
    if (claimed_by.empty()) throw std::invalid_argument("claimed_by is required");
    auto current = handoff_get(db, handoff_id);
    if (current.status == "claimed" && current.claimed_by == claimed_by) return current;
    if (current.status != "pending")
        throw std::invalid_argument("handoff must be pending before it can be claimed");

    auto updated =
        db.conn().Query("UPDATE handoffs SET status = 'claimed', claimed_by = '" + sq(claimed_by) +
                        "', claimed_at = now() WHERE id = " + std::to_string(handoff_id) +
                        " AND status = 'pending' RETURNING id");
    if (updated->HasError() || updated->RowCount() == 0)
        throw std::runtime_error("handoff claim lost its state transition race");
    return handoff_get(db, handoff_id);
}

Handoff handoff_complete(Database& db, int64_t handoff_id, const std::string& claimed_by,
                         const std::string& result) {
    if (claimed_by.empty()) throw std::invalid_argument("claimed_by is required");
    auto current = handoff_get(db, handoff_id);
    if (current.status == "completed" && current.claimed_by == claimed_by) return current;
    if (current.status != "claimed")
        throw std::invalid_argument("handoff must be claimed before it can be completed");
    if (current.claimed_by != claimed_by)
        throw std::invalid_argument("handoff can only be completed by its claimant");

    auto updated =
        db.conn().Query("UPDATE handoffs SET status = 'completed', result = '" + sq(result) +
                        "', completed_at = now() WHERE id = " + std::to_string(handoff_id) +
                        " AND status = 'claimed' AND "
                        "claimed_by = '" +
                        sq(claimed_by) + "' RETURNING id");
    if (updated->HasError() || updated->RowCount() == 0)
        throw std::runtime_error("handoff completion lost its state transition race");
    return handoff_get(db, handoff_id);
}

Handoff handoff_cancel(Database& db, int64_t handoff_id) {
    auto current = handoff_get(db, handoff_id);
    if (current.status == "cancelled") return current;
    if (current.status == "completed")
        throw std::invalid_argument("completed handoff cannot be cancelled");

    auto updated = db.conn().Query("UPDATE handoffs SET status = 'cancelled', completed_at = now() "
                                   "WHERE id = " +
                                   std::to_string(handoff_id) +
                                   " AND status IN ('pending', 'claimed') RETURNING id");
    if (updated->HasError() || updated->RowCount() == 0)
        throw std::runtime_error("handoff cancellation lost its state transition race");
    return handoff_get(db, handoff_id);
}

// ── Turn operations ───────────────────────────────────────────────────────────

int64_t turn_add(Database& db, EmbeddingModel* model, int64_t session_id, const std::string& role,
                 const std::string& content) {
    int64_t tid = -1;

    if (model) {
        auto emb = model->embed(content);
        auto ins =
            db.conn().Query("INSERT INTO turns (id, session_id, role, content, embedding) VALUES ("
                            "nextval('seq_id'), " +
                            std::to_string(session_id) + ", '" + sq(role) + "', '" + sq(content) +
                            "', " + vec_literal(emb, model->dims()) + ") RETURNING id");
        if (ins->HasError()) throw std::runtime_error("turn_add: " + ins->GetError());
        tid = ins->GetValue<int64_t>(0, 0);
    } else {
        auto ins = db.conn().Query("INSERT INTO turns (id, session_id, role, content) VALUES "
                                   "(nextval('seq_id'), " +
                                   std::to_string(session_id) + ", '" + sq(role) + "', '" +
                                   sq(content) + "') RETURNING id");
        if (ins->HasError()) throw std::runtime_error("turn_add: " + ins->GetError());
        tid = ins->GetValue<int64_t>(0, 0);
    }

    auto_anchor(db, tid, content);
    return tid;
}

void auto_anchor(Database& db, int64_t turn_id, const std::string& content) {
    // ── Pass 1: detect source file path patterns ──────────────────────────────
    static const std::regex file_re(
        R"([a-zA-Z0-9_\-/]+\.(?:ts|tsx|js|jsx|mjs|cjs|py|rs|go|cs|cpp|cc|cxx|hpp|h|java|php|dart|kt|kts|sh|bash|vue))",
        std::regex::optimize);

    std::unordered_set<int64_t> seen_files;
    auto file_begin = std::sregex_iterator(content.begin(), content.end(), file_re);
    auto file_end = std::sregex_iterator();

    for (auto it = file_begin; it != file_end; ++it) {
        std::string match = (*it)[0].str();
        // Extract just the basename portion for LIKE matching
        auto slash = match.rfind('/');
        std::string basename = (slash == std::string::npos) ? match : match.substr(slash + 1);

        auto res =
            db.conn().Query("SELECT id FROM files WHERE path LIKE '%" + sq(basename) + "' LIMIT 1");
        if (!res->HasError() && res->RowCount() > 0) {
            int64_t fid = res->GetValue<int64_t>(0, 0);
            if (seen_files.count(fid)) continue;
            seen_files.insert(fid);

            db.conn().Query("INSERT INTO turn_anchors (id, turn_id, file_id, kind) VALUES ("
                            "nextval('seq_id'), " +
                            std::to_string(turn_id) + ", " + std::to_string(fid) + ", 'mentions')");
        }
    }

    // ── Pass 2: detect symbol names using word-boundary scan ─────────────────
    // Load top-500 symbol names from graph (most-referenced first)
    auto sym_res = db.conn().Query("SELECT DISTINCT s.name FROM symbols s"
                                   " JOIN edges e ON (e.from_symbol = s.id OR e.to_symbol = s.id)"
                                   " GROUP BY s.name ORDER BY COUNT(*) DESC LIMIT 500");

    if (sym_res->HasError()) return;

    std::unordered_set<int64_t> seen_symbols;
    for (duckdb::idx_t i = 0; i < sym_res->RowCount(); i++) {
        std::string sym_name = sym_res->GetValue(0, i).ToString();
        if (sym_name.size() < 3) continue; // skip very short names — too many false positives

        // Word-boundary check using regex
        try {
            std::regex sym_re("\\b" + sym_name + "\\b", std::regex::optimize);
            if (!std::regex_search(content, sym_re)) continue;
        } catch (...) {
            continue;
        }

        auto sr =
            db.conn().Query("SELECT id FROM symbols WHERE name = '" + sq(sym_name) + "' LIMIT 1");
        if (!sr->HasError() && sr->RowCount() > 0) {
            int64_t sid = sr->GetValue<int64_t>(0, 0);
            if (seen_symbols.count(sid)) continue;
            seen_symbols.insert(sid);

            db.conn().Query("INSERT INTO turn_anchors (id, turn_id, symbol_id, kind) VALUES ("
                            "nextval('seq_id'), " +
                            std::to_string(turn_id) + ", " + std::to_string(sid) + ", 'mentions')");
        }
    }
}

int64_t anchor_link(Database& db, int64_t turn_id, int64_t file_id, int64_t symbol_id,
                    const std::string& kind) {
    auto ins = db.conn().Query(
        "INSERT INTO turn_anchors (id, turn_id, file_id, symbol_id, kind) VALUES ("
        "nextval('seq_id'), " +
        std::to_string(turn_id) + ", " + (file_id > 0 ? std::to_string(file_id) : "NULL") + ", " +
        (symbol_id > 0 ? std::to_string(symbol_id) : "NULL") + ", '" + sq(kind) +
        "') RETURNING id");
    if (ins->HasError()) throw std::runtime_error("anchor_link: " + ins->GetError());
    return ins->GetValue<int64_t>(0, 0);
}

std::vector<Anchor> turn_get_anchors(Database& db, int64_t turn_id) {
    auto res = db.conn().Query("SELECT id, turn_id,"
                               "  COALESCE(file_id, -1),"
                               "  COALESCE(symbol_id, -1),"
                               "  kind"
                               " FROM turn_anchors WHERE turn_id = " +
                               std::to_string(turn_id));
    if (res->HasError()) return {};

    std::vector<Anchor> out;
    for (duckdb::idx_t i = 0; i < res->RowCount(); i++) {
        Anchor a;
        a.id = res->GetValue<int64_t>(0, i);
        a.turn_id = res->GetValue<int64_t>(1, i);
        a.file_id = res->GetValue<int64_t>(2, i);
        a.symbol_id = res->GetValue<int64_t>(3, i);
        a.kind = res->GetValue(4, i).ToString();
        out.push_back(a);
    }
    return out;
}

// ── Semantic search ───────────────────────────────────────────────────────────

std::vector<TurnHit> turn_search(Database& db, EmbeddingModel& model, const std::string& query,
                                 int limit, int64_t thread_id) {
    auto qvec = model.embed(query);
    std::string vec_str = vec_literal(qvec, model.dims());

    std::string where = thread_id >= 0 ? " AND s.thread_id = " + std::to_string(thread_id) : "";

    std::string sql = "SELECT t.id, t.session_id, t.role, t.content, CAST(t.ts AS VARCHAR),"
                      "       array_cosine_similarity(t.embedding, " +
                      vec_str +
                      ") AS score,"
                      "       COALESCE(s.label,''), th.name"
                      " FROM turns t"
                      " JOIN sessions s ON s.id = t.session_id"
                      " JOIN threads th ON th.id = s.thread_id"
                      " WHERE t.embedding IS NOT NULL" +
                      where +
                      " ORDER BY score DESC"
                      " LIMIT " +
                      std::to_string(limit);

    auto res = db.conn().Query(sql);
    if (res->HasError()) {
        std::cerr << "[axon] turn_search error: " << res->GetError() << "\n";
        return {};
    }

    std::vector<TurnHit> out;
    for (duckdb::idx_t i = 0; i < res->RowCount(); i++) {
        Turn t;
        t.id = res->GetValue<int64_t>(0, i);
        t.session_id = res->GetValue<int64_t>(1, i);
        t.role = res->GetValue(2, i).ToString();
        t.content = res->GetValue(3, i).ToString();
        t.ts = res->GetValue(4, i).ToString();
        TurnHit h;
        h.turn = t;
        h.score = (float)res->GetValue<double>(5, i);
        h.session_label = res->GetValue(6, i).ToString();
        h.thread_name = res->GetValue(7, i).ToString();
        out.push_back(h);
    }
    return out;
}

std::vector<TurnHit> turns_for_files(Database& db, EmbeddingModel& model, const std::string& query,
                                     const std::vector<int64_t>& file_ids, int token_budget) {
    if (file_ids.empty()) return {};

    // Build IN clause
    std::string in_clause;
    for (size_t i = 0; i < file_ids.size(); i++) {
        if (i) in_clause += ",";
        in_clause += std::to_string(file_ids[i]);
    }

    // Fetch turns anchored to these files, ordered by semantic similarity to query
    auto qvec = model.embed(query);
    std::string vec_str = vec_literal(qvec, model.dims());

    std::string sql =
        "SELECT DISTINCT t.id, t.session_id, t.role, t.content, CAST(t.ts AS VARCHAR),"
        "       CASE WHEN t.embedding IS NOT NULL"
        "            THEN array_cosine_similarity(t.embedding, " +
        vec_str +
        ")"
        "            ELSE 0.0 END AS score,"
        "       COALESCE(s.label,''), th.name"
        " FROM turns t"
        " JOIN turn_anchors ta ON ta.turn_id = t.id"
        " JOIN sessions s  ON s.id  = t.session_id"
        " JOIN threads  th ON th.id = s.thread_id"
        " WHERE ta.file_id IN (" +
        in_clause +
        ")"
        " ORDER BY score DESC"
        " LIMIT 20";

    auto res = db.conn().Query(sql);
    if (res->HasError()) return {};

    std::vector<TurnHit> out;
    int consumed = 0;
    for (duckdb::idx_t i = 0; i < res->RowCount(); i++) {
        Turn t;
        t.id = res->GetValue<int64_t>(0, i);
        t.session_id = res->GetValue<int64_t>(1, i);
        t.role = res->GetValue(2, i).ToString();
        t.content = res->GetValue(3, i).ToString();
        t.ts = res->GetValue(4, i).ToString();

        int tokens = (int)((t.content.size() + 3) / 4);
        if (consumed + tokens > token_budget) break;
        consumed += tokens;

        TurnHit h;
        h.turn = t;
        h.score = (float)res->GetValue<double>(5, i);
        h.session_label = res->GetValue(6, i).ToString();
        h.thread_name = res->GetValue(7, i).ToString();
        out.push_back(h);
    }
    return out;
}

// ── Embed pipeline ────────────────────────────────────────────────────────────

int embed_pending_turns(Database& db, EmbeddingModel& model, int limit) {
    auto res = db.conn().Query("SELECT id, content FROM turns WHERE embedding IS NULL LIMIT " +
                               std::to_string(limit));
    if (res->HasError()) return 0;
    if (res->RowCount() == 0) return 0;

    std::vector<int64_t> ids;
    std::vector<std::string> texts;
    for (duckdb::idx_t i = 0; i < res->RowCount(); i++) {
        ids.push_back(res->GetValue<int64_t>(0, i));
        texts.push_back(res->GetValue(1, i).ToString());
    }

    auto embeddings = model.embed_batch(texts);
    int dims = model.dims();

    for (size_t i = 0; i < ids.size(); i++) {
        db.conn().Query("UPDATE turns SET embedding = " + vec_literal(embeddings[i], dims) +
                        " WHERE id = " + std::to_string(ids[i]));
    }
    return (int)ids.size();
}

// ── Symbol name cache ─────────────────────────────────────────────────────────

std::unordered_set<std::string> load_symbol_name_cache(Database& db, int limit) {
    auto res = db.conn().Query("SELECT DISTINCT s.name FROM symbols s"
                               " LEFT JOIN edges e ON (e.from_symbol = s.id OR e.to_symbol = s.id)"
                               " GROUP BY s.name ORDER BY COUNT(e.id) DESC LIMIT " +
                               std::to_string(limit));

    std::unordered_set<std::string> cache;
    if (res->HasError()) return cache;
    for (duckdb::idx_t i = 0; i < res->RowCount(); i++)
        cache.insert(res->GetValue(0, i).ToString());
    return cache;
}

} // namespace axon
