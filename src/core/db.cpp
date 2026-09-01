#include "db.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <regex>
#include <sstream>

namespace axon {

namespace {

bool canonical_uuid(const std::string& value) {
    static const std::regex pattern(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    return std::regex_match(value, pattern);
}

std::string uuid_v4() {
    std::random_device random;
    std::uniform_int_distribution<unsigned int> byte(0, 255);
    unsigned char bytes[16];
    for (auto& value : bytes) value = static_cast<unsigned char>(byte(random));
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) output << '-';
        output << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return output.str();
}

std::optional<std::string>
repository_identity_from_contract(const std::filesystem::path& db_path) {
    const auto project_root = db_path.parent_path().filename() == ".axon"
                                  ? db_path.parent_path().parent_path()
                                  : db_path.parent_path();
    std::ifstream contract(project_root / "repository-contract.yaml");
    const bool contract_exists = static_cast<bool>(contract);
    std::string line;
    bool schema_seen = false;
    bool identity_seen = false;
    std::optional<std::string> repository_id;
    static const std::regex top_level_field(R"(^([A-Za-z_][A-Za-z0-9_-]*)\s*:\s*(.*)$)");
    auto canonical_value = [](std::string value) {
        const auto comment = value.find('#');
        if (comment != std::string::npos) value.erase(comment);
        const auto first = value.find_first_not_of(" \t\r");
        if (first == std::string::npos) return std::string{};
        const auto last = value.find_last_not_of(" \t\r");
        return value.substr(first, last - first + 1);
    };
    std::smatch match;
    while (std::getline(contract, line)) {
        if (!std::regex_match(line, match, top_level_field)) continue;
        const std::string field = match[1].str();
        const std::string value = canonical_value(match[2].str());
        if (field == "schema_version") {
            if (schema_seen || value != "repository-contract/v1")
                throw std::runtime_error(
                    "repository-contract.yaml has a duplicate or unsupported schema_version");
            schema_seen = true;
        } else if (field == "repository_id") {
            if (repository_id || !canonical_uuid(value))
                throw std::runtime_error(
                    "repository-contract.yaml has a duplicate or invalid repository_id");
            repository_id = value;
        } else if (field == "identity") {
            // The canonical v1 contract uses a block mapping. Reject scalar identity claims; Axon
            // does not attempt permissive, partial YAML interpretation at this authority boundary.
            if (identity_seen || !value.empty())
                throw std::runtime_error(
                    "repository-contract.yaml identity must be a single block mapping");
            identity_seen = true;
        }
    }
    if (contract_exists && (!schema_seen || !repository_id))
        throw std::runtime_error(
            "repository-contract.yaml must declare repository-contract/v1 and one canonical "
            "repository_id");
    if (repository_id) return repository_id;
    return std::nullopt;
}

} // namespace

bool is_database_lock_error(const std::string& message) {
    std::string lower;
    lower.reserve(message.size());
    for (unsigned char ch : message)
        lower.push_back(static_cast<char>(std::tolower(ch)));

    return lower.find("conflicting lock") != std::string::npos ||
           lower.find("could not set lock") != std::string::npos ||
           lower.find("database is locked") != std::string::npos ||
           lower.find("failed to create file lock") != std::string::npos ||
           (lower.find("lock") != std::string::npos && lower.find("duckdb") != std::string::npos);
}

std::string database_open_error_message(const std::filesystem::path& db_path,
                                        const std::exception& error) {
    const std::string detail = error.what();
    if (is_database_lock_error(detail)) {
        return "DuckDB index is locked: " + db_path.string() +
               "\n"
               "Another axon process, usually `axon serve` or `axon web`, is using this index. "
               "Stop that process and retry.\n"
               "DuckDB detail: " +
               detail;
    }
    return "Failed to open DuckDB index: " + db_path.string() + "\n" + detail;
}

Database::Database(const std::filesystem::path& db_path)
    : db_path_(db_path), db_(db_path.string()), conn_(std::make_unique<duckdb::Connection>(db_)) {
    run_migrations();
}

void Database::exec(const std::string& sql) {
    auto res = conn_->Query(sql);
    if (res->HasError())
        throw std::runtime_error("DuckDB exec error: " + res->GetError() +
                                 "\nSQL: " + sql.substr(0, 80));
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
    try {
        exec("ALTER TABLE files ADD COLUMN skeleton VARCHAR");
    } catch (...) {
    }

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
    // Unresolved imports are still useful capability evidence. They carry only a module
    // specifier (never source text) and are removed with their owning file.
    exec("CREATE TABLE IF NOT EXISTS external_dependencies ("
         "  from_file BIGINT NOT NULL,"
         "  specifier VARCHAR NOT NULL,"
         "  kind VARCHAR NOT NULL,"
         "  PRIMARY KEY(from_file, specifier, kind)"
         ")");
    // Additive, metadata-only capability evidence for read-only portfolio projection.
    exec("CREATE TABLE IF NOT EXISTS capability_contexts ("
         "  file_id BIGINT PRIMARY KEY,"
         "  bounded_context VARCHAR NOT NULL"
         ")");
    exec("CREATE TABLE IF NOT EXISTS capability_ast_fingerprints ("
         "  file_id BIGINT PRIMARY KEY,"
         "  value VARCHAR NOT NULL"
         ")");

    exec("CREATE TABLE IF NOT EXISTS observations ("
         "  id         BIGINT PRIMARY KEY,"
         "  content    VARCHAR NOT NULL,"
         "  file_path  VARCHAR,"
         "  embedding  FLOAT[768],"
         "  authority  DOUBLE NOT NULL DEFAULT 1.0,"
         "  created_at TIMESTAMP NOT NULL DEFAULT now()"
         ")");
    try {
        exec("ALTER TABLE observations ADD COLUMN authority DOUBLE DEFAULT 1.0");
    } catch (...) {
    }

    // Observation tags live in a child table so existing databases upgrade
    // without rewriting the observations table. The composite primary key
    // also makes repeated tags idempotent.
    exec("CREATE TABLE IF NOT EXISTS observation_tags ("
         "  observation_id BIGINT NOT NULL,"
         "  tag            VARCHAR NOT NULL,"
         "  PRIMARY KEY (observation_id, tag)"
         ")");
    try {
        exec("CREATE INDEX IF NOT EXISTS idx_observation_tags_tag ON observation_tags(tag)");
    } catch (...) {
    }

    // Index for O(1) symbol-level edge resolution
    try {
        exec("CREATE INDEX IF NOT EXISTS idx_symbols_file_name ON symbols(file_id, name)");
    } catch (...) {
    }

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
    // epoch shifts and old entries become unreachable — capsule_cache_prune
    // reaps them after every (re)index that moved the epoch. Cache hits avoid
    // the expensive embedding + ranking + skeletonization roundtrip.
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
    try {
        exec("ALTER TABLE telemetry_events ADD COLUMN layer VARCHAR DEFAULT 'unknown'");
    } catch (...) {
    }
    try {
        exec("CREATE INDEX IF NOT EXISTS idx_telemetry_events_created ON "
             "telemetry_events(created_at)");
    } catch (...) {
    }
    try {
        exec("CREATE INDEX IF NOT EXISTS idx_telemetry_events_layer ON telemetry_events(layer)");
    } catch (...) {
    }

    exec("CREATE TABLE IF NOT EXISTS ccr_artifacts ("
         "  artifact_id     VARCHAR PRIMARY KEY,"
         "  kind            VARCHAR NOT NULL,"
         "  source_ref      VARCHAR NOT NULL,"
         "  content         VARCHAR NOT NULL,"
         "  token_estimate  BIGINT NOT NULL DEFAULT 0,"
         "  created_at      TIMESTAMP NOT NULL DEFAULT now()"
         ")");
    try {
        exec("CREATE INDEX IF NOT EXISTS idx_ccr_artifacts_created ON ccr_artifacts(created_at)");
    } catch (...) {
    }
    try {
        exec("CREATE INDEX IF NOT EXISTS idx_ccr_artifacts_source ON ccr_artifacts(source_ref)");
    } catch (...) {
    }

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
    try {
        exec("ALTER TABLE sessions ADD COLUMN idempotency_key VARCHAR");
    } catch (...) {
    }
    try {
        exec("CREATE INDEX IF NOT EXISTS idx_sessions_thread ON sessions(thread_id)");
    } catch (...) {
    }
    try {
        exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_sessions_idempotency "
             "ON sessions(thread_id, idempotency_key)");
    } catch (...) {
    }

    exec("CREATE TABLE IF NOT EXISTS turns ("
         "  id         BIGINT PRIMARY KEY,"
         "  session_id BIGINT NOT NULL,"
         "  role       VARCHAR NOT NULL DEFAULT 'user',"
         "  content    VARCHAR NOT NULL,"
         "  ts         TIMESTAMP NOT NULL DEFAULT now(),"
         "  embedding  FLOAT[768]"
         ")");
    try {
        exec("CREATE INDEX IF NOT EXISTS idx_turns_session ON turns(session_id, ts)");
    } catch (...) {
    }

    exec("CREATE TABLE IF NOT EXISTS turn_anchors ("
         "  id        BIGINT PRIMARY KEY,"
         "  turn_id   BIGINT NOT NULL,"
         "  file_id   BIGINT,"
         "  symbol_id BIGINT,"
         "  kind      VARCHAR NOT NULL DEFAULT 'mentions'"
         ")");
    try {
        exec("CREATE INDEX IF NOT EXISTS idx_anchors_turn   ON turn_anchors(turn_id)");
    } catch (...) {
    }
    try {
        exec("CREATE INDEX IF NOT EXISTS idx_anchors_file   ON turn_anchors(file_id)");
    } catch (...) {
    }
    try {
        exec("CREATE INDEX IF NOT EXISTS idx_anchors_symbol ON turn_anchors(symbol_id)");
    } catch (...) {
    }

    exec("CREATE TABLE IF NOT EXISTS handoffs ("
         "  id                BIGINT PRIMARY KEY,"
         "  source_session_id BIGINT,"
         "  target_agent      VARCHAR NOT NULL,"
         "  project_root      VARCHAR NOT NULL,"
         "  working_directory VARCHAR NOT NULL,"
         "  objective         VARCHAR NOT NULL,"
         "  context           VARCHAR NOT NULL DEFAULT '',"
         "  status            VARCHAR NOT NULL DEFAULT 'pending',"
         "  claimed_by        VARCHAR,"
         "  result            VARCHAR,"
         "  idempotency_key   VARCHAR,"
         "  created_at        TIMESTAMP NOT NULL DEFAULT now(),"
         "  claimed_at        TIMESTAMP,"
         "  completed_at      TIMESTAMP"
         ")");
    try {
        exec("CREATE INDEX IF NOT EXISTS idx_handoffs_target_status "
             "ON handoffs(target_agent, status)");
    } catch (...) {
    }
    try {
        exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_handoffs_idempotency "
             "ON handoffs(source_session_id, idempotency_key)");
    } catch (...) {
    }

    // Portfolio index journal v1. These tables are additive: older binaries ignore them.
    // Identity is persisted once per physical database. A copied database deliberately keeps its
    // stream id so the registry/projector can quarantine a duplicate publisher binding.
    exec("CREATE TABLE IF NOT EXISTS schema_migrations ("
         "  component VARCHAR NOT NULL,"
         "  version INTEGER NOT NULL,"
         "  applied_at TIMESTAMP NOT NULL DEFAULT now(),"
         "  checksum VARCHAR NOT NULL,"
         "  PRIMARY KEY(component, version)"
         ")");
    exec("CREATE TABLE IF NOT EXISTS index_metadata ("
         "  singleton BOOLEAN PRIMARY KEY,"
         "  repository_id VARCHAR NOT NULL,"
         "  index_stream_id VARCHAR NOT NULL,"
         "  schema_version VARCHAR NOT NULL,"
         "  current_epoch VARCHAR NOT NULL DEFAULT '',"
         "  current_manifest VARCHAR NOT NULL DEFAULT '',"
         "  removed BOOLEAN NOT NULL DEFAULT false"
         ")");
    try {
        exec("ALTER TABLE index_metadata ADD COLUMN removed BOOLEAN DEFAULT false");
    } catch (...) {
    }
    exec("CREATE TABLE IF NOT EXISTS index_events ("
         "  repository_id VARCHAR NOT NULL,"
         "  index_stream_id VARCHAR NOT NULL,"
         "  sequence UBIGINT NOT NULL,"
         "  event_id VARCHAR NOT NULL,"
         "  schema_version VARCHAR NOT NULL,"
         "  event_type VARCHAR NOT NULL,"
         "  index_epoch VARCHAR NOT NULL,"
         "  previous_epoch VARCHAR,"
         "  source_ref VARCHAR,"
         "  occurred_at TIMESTAMP NOT NULL DEFAULT now(),"
         "  manifest_hash VARCHAR,"
         "  payload_json VARCHAR NOT NULL,"
         "  PRIMARY KEY(index_stream_id, sequence),"
         "  UNIQUE(event_id)"
         ")");
    exec("CREATE TABLE IF NOT EXISTS index_tombstones ("
         "  repository_id VARCHAR NOT NULL,"
         "  index_stream_id VARCHAR NOT NULL,"
         "  entity_kind VARCHAR NOT NULL,"
         "  entity_key VARCHAR NOT NULL,"
         "  deleted_sequence UBIGINT NOT NULL,"
         "  deleted_epoch VARCHAR NOT NULL,"
         "  deleted_at TIMESTAMP NOT NULL,"
         "  PRIMARY KEY(index_stream_id, entity_kind, entity_key)"
         ")");

    auto identity = conn_->Query("SELECT COUNT(*) FROM index_metadata WHERE singleton=true");
    require_ok(identity, "inspect index identity");
    if (identity->GetValue<int64_t>(0, 0) == 0) {
        const std::string repository_id =
            repository_identity_from_contract(db_path_).value_or(uuid_v4());
        const std::string stream_id = uuid_v4();
        exec("INSERT INTO index_metadata(singleton,repository_id,index_stream_id,schema_version) "
             "VALUES (true,'" +
             repository_id + "','" + stream_id + "','axon/index-metadata/v1')");
    } else {
        const auto contract_repository_id = repository_identity_from_contract(db_path_);
        if (contract_repository_id) {
            auto persisted = conn_->Query(
                "SELECT repository_id FROM index_metadata WHERE singleton=true");
            require_ok(persisted, "read persisted index identity");
            const std::string persisted_repository_id = persisted->GetValue(0, 0).ToString();
            if (*contract_repository_id != persisted_repository_id) {
                throw std::runtime_error(
                    "repository-contract.yaml repository_id differs from the persisted index "
                    "identity; use the governed repository reidentification operation");
            }
        }
    }
    exec("INSERT INTO schema_migrations(component,version,checksum) "
         "VALUES ('portfolio-index-journal',1,'axon/portfolio-index-journal/v1') "
         "ON CONFLICT(component,version) DO NOTHING");
}

} // namespace axon
