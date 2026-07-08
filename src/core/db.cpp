#include "db.hpp"
#include <algorithm>
#include <cctype>
#include <iostream>

namespace axon {

bool is_database_lock_error(const std::string& message) {
    std::string lower;
    lower.reserve(message.size());
    for (unsigned char ch : message)
        lower.push_back(static_cast<char>(std::tolower(ch)));

    return lower.find("conflicting lock") != std::string::npos ||
           lower.find("could not set lock") != std::string::npos ||
           lower.find("database is locked") != std::string::npos ||
           lower.find("failed to create file lock") != std::string::npos ||
           (lower.find("lock") != std::string::npos &&
            lower.find("duckdb") != std::string::npos);
}

std::string database_open_error_message(const std::filesystem::path& db_path,
                                        const std::exception& error) {
    const std::string detail = error.what();
    if (is_database_lock_error(detail)) {
        return "DuckDB index is locked: " + db_path.string() + "\n"
               "Another axon process, usually `axon serve` or `axon web`, is using this index. "
               "Stop that process and retry.\n"
               "DuckDB detail: " + detail;
    }
    return "Failed to open DuckDB index: " + db_path.string() + "\n" + detail;
}

Database::Database(const std::filesystem::path& db_path)
    : db_(db_path.string()), conn_(std::make_unique<duckdb::Connection>(db_))
{
    run_migrations();
}

void Database::exec(const std::string& sql) {
    auto res = conn_->Query(sql);
    if (res->HasError())
        throw std::runtime_error("DuckDB exec error: " + res->GetError() + "\nSQL: " + sql.substr(0, 80));
}

duckdb::MaterializedQueryResult& Database::query(const std::string& sql) {
    last_result_ = conn_->Query(sql);
    if (last_result_->HasError())
        throw std::runtime_error("DuckDB query error: " + last_result_->GetError());
    return *last_result_;
}

void Database::run_migrations() {
    exec("CREATE TABLE IF NOT EXISTS files ("
         "  id        BIGINT PRIMARY KEY,"
         "  path      VARCHAR NOT NULL UNIQUE,"
         "  language  VARCHAR NOT NULL,"
         "  hash      VARCHAR NOT NULL,"
         "  indexed_at TIMESTAMP NOT NULL DEFAULT now(),"
         "  byte_size BIGINT NOT NULL DEFAULT 0,"
         "  skeleton  VARCHAR"
         ")");
    // Add skeleton column if upgrading from older schema
    try { exec("ALTER TABLE files ADD COLUMN skeleton VARCHAR"); } catch (...) {}

    exec("CREATE SEQUENCE IF NOT EXISTS seq_id START 1");

    exec("CREATE TABLE IF NOT EXISTS symbols ("
         "  id         BIGINT PRIMARY KEY,"
         "  file_id    BIGINT NOT NULL,"
         "  name       VARCHAR NOT NULL,"
         "  kind       VARCHAR NOT NULL,"
         "  start_line INTEGER NOT NULL,"
         "  end_line   INTEGER NOT NULL,"
         "  signature  VARCHAR,"
         "  docstring  VARCHAR,"
         "  embedding  FLOAT[768]"
         ")");

    exec("CREATE TABLE IF NOT EXISTS edges ("
         "  id          BIGINT PRIMARY KEY,"
         "  from_file   BIGINT NOT NULL,"
         "  to_file     BIGINT NOT NULL,"
         "  from_symbol BIGINT,"
         "  to_symbol   BIGINT,"
         "  kind        VARCHAR NOT NULL DEFAULT 'imports'"
         ")");

    exec("CREATE TABLE IF NOT EXISTS observations ("
         "  id         BIGINT PRIMARY KEY,"
         "  content    VARCHAR NOT NULL,"
         "  file_path  VARCHAR,"
         "  embedding  FLOAT[768],"
         "  created_at TIMESTAMP NOT NULL DEFAULT now()"
         ")");

    // Index for O(1) symbol-level edge resolution
    try { exec("CREATE INDEX IF NOT EXISTS idx_symbols_file_name ON symbols(file_id, name)"); } catch (...) {}

    // Routes table (created on demand by index_routes, but define here for upgrade path)
    exec("CREATE TABLE IF NOT EXISTS routes ("
         "  id           BIGINT PRIMARY KEY,"
         "  method       VARCHAR NOT NULL,"
         "  path         VARCHAR NOT NULL,"
         "  handler_file VARCHAR NOT NULL,"
         "  framework    VARCHAR NOT NULL DEFAULT 'unknown',"
         "  file_id      BIGINT"
         ")");

    // Capsule cache (W2.T01). Keyed by BLAKE3(query + project_epoch) where
    // project_epoch = max(files.indexed_at). When the index changes, the
    // epoch shifts and old entries become unreachable — they sit dead in the
    // table until the periodic prune below reaps them. Cache hits avoid the
    // expensive embedding + ranking + skeletonization roundtrip.
    exec("CREATE TABLE IF NOT EXISTS capsule_cache ("
         "  query_hash  VARCHAR PRIMARY KEY,"
         "  epoch       VARCHAR NOT NULL,"
         "  payload     VARCHAR NOT NULL,"
         "  created_at  TIMESTAMP NOT NULL DEFAULT now()"
         ")");

    exec("CREATE TABLE IF NOT EXISTS telemetry_events ("
         "  id                         BIGINT PRIMARY KEY,"
         "  type                       VARCHAR NOT NULL,"
         "  origin                     VARCHAR NOT NULL,"
         "  layer                      VARCHAR NOT NULL DEFAULT 'unknown',"
         "  created_at                 TIMESTAMP NOT NULL DEFAULT now(),"
         "  latency_ms                 BIGINT NOT NULL DEFAULT 0,"
         "  tokens_estimated           BIGINT NOT NULL DEFAULT 0,"
         "  baseline_tokens_estimated  BIGINT NOT NULL DEFAULT 0,"
         "  tokens_saved               BIGINT NOT NULL DEFAULT 0,"
         "  cache_hit                  BOOLEAN NOT NULL DEFAULT false"
         ")");
    // DuckDB rejects ADD COLUMN with a NOT NULL constraint ("Adding columns
    // with constraints not yet supported"), so the migration must stay
    // nullable — with NOT NULL here the ALTER always failed and every DB
    // created before this column silently lost ALL telemetry (the INSERT
    // names `layer` and errored). Fresh DBs still get NOT NULL from the
    // CREATE TABLE above.
    try { exec("ALTER TABLE telemetry_events ADD COLUMN layer VARCHAR DEFAULT 'unknown'"); } catch (...) {}
    try { exec("CREATE INDEX IF NOT EXISTS idx_telemetry_events_created ON telemetry_events(created_at)"); } catch (...) {}
    try { exec("CREATE INDEX IF NOT EXISTS idx_telemetry_events_layer ON telemetry_events(layer)"); } catch (...) {}

    exec("CREATE TABLE IF NOT EXISTS ccr_artifacts ("
         "  artifact_id     VARCHAR PRIMARY KEY,"
         "  kind            VARCHAR NOT NULL,"
         "  source_ref      VARCHAR NOT NULL,"
         "  content         VARCHAR NOT NULL,"
         "  token_estimate  BIGINT NOT NULL DEFAULT 0,"
         "  created_at      TIMESTAMP NOT NULL DEFAULT now()"
         ")");
    try { exec("CREATE INDEX IF NOT EXISTS idx_ccr_artifacts_created ON ccr_artifacts(created_at)"); } catch (...) {}
    try { exec("CREATE INDEX IF NOT EXISTS idx_ccr_artifacts_source ON ccr_artifacts(source_ref)"); } catch (...) {}

    // ── Dialogue Layer ────────────────────────────────────────────────────────
    // Structured conversation memory: Thread → Session → Turn, with automatic
    // code anchoring via the project's file/symbol graph.

    exec("CREATE TABLE IF NOT EXISTS threads ("
         "  id         BIGINT PRIMARY KEY,"
         "  name       VARCHAR NOT NULL UNIQUE,"
         "  kind       VARCHAR NOT NULL DEFAULT 'project',"
         "  created_at TIMESTAMP NOT NULL DEFAULT now()"
         ")");

    exec("CREATE TABLE IF NOT EXISTS sessions ("
         "  id               BIGINT PRIMARY KEY,"
         "  thread_id        BIGINT NOT NULL,"
         "  label            VARCHAR,"
         "  started_at       TIMESTAMP NOT NULL DEFAULT now(),"
         "  ended_at         TIMESTAMP,"
         "  digest           VARCHAR,"
         "  digest_embedding FLOAT[768]"
         ")");
    try { exec("CREATE INDEX IF NOT EXISTS idx_sessions_thread ON sessions(thread_id)"); } catch (...) {}

    exec("CREATE TABLE IF NOT EXISTS turns ("
         "  id         BIGINT PRIMARY KEY,"
         "  session_id BIGINT NOT NULL,"
         "  role       VARCHAR NOT NULL DEFAULT 'user',"
         "  content    VARCHAR NOT NULL,"
         "  ts         TIMESTAMP NOT NULL DEFAULT now(),"
         "  embedding  FLOAT[768]"
         ")");
    try { exec("CREATE INDEX IF NOT EXISTS idx_turns_session ON turns(session_id, ts)"); } catch (...) {}

    exec("CREATE TABLE IF NOT EXISTS turn_anchors ("
         "  id        BIGINT PRIMARY KEY,"
         "  turn_id   BIGINT NOT NULL,"
         "  file_id   BIGINT,"
         "  symbol_id BIGINT,"
         "  kind      VARCHAR NOT NULL DEFAULT 'mentions'"
         ")");
    try { exec("CREATE INDEX IF NOT EXISTS idx_anchors_turn   ON turn_anchors(turn_id)");   } catch (...) {}
    try { exec("CREATE INDEX IF NOT EXISTS idx_anchors_file   ON turn_anchors(file_id)");   } catch (...) {}
    try { exec("CREATE INDEX IF NOT EXISTS idx_anchors_symbol ON turn_anchors(symbol_id)"); } catch (...) {}
}

} // namespace axon
