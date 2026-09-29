#include "index_journal.hpp"

#include "core/db.hpp"
#include <blake3.h>
#include <nlohmann/json.hpp>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <unordered_set>

namespace axon::portfolio {
namespace {

std::mutex failpoint_mutex;
std::string active_failpoint;

const std::unordered_set<std::string> event_types = {
    "IndexSnapshotCompleted", "IndexFilesUpdated",  "IndexFilesDeleted",      "IndexSymbolsUpdated",
    "IndexContractsUpdated",  "IndexRoutesUpdated", "RepositoryReidentified", "RepositoryRemoved"};
const std::unordered_set<std::string> entity_kinds = {
    "file",   "symbol", "contract", "route",      "handler",   "event",
    "schema", "dto",    "test",     "dependency", "tombstone", "repository"};
const std::unordered_set<std::string> operations = {"upsert", "delete", "snapshot"};
const std::unordered_set<std::string> identity_reasons = {"contract-adopted", "collision-repaired",
                                                          "repository-moved", "owner-approved"};

bool canonical_uuid(const std::string& value) {
    static const std::regex pattern(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    return std::regex_match(value, pattern);
}

std::string digest(const std::string& input) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, input.data(), input.size());
    uint8_t bytes[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&hasher, bytes, sizeof(bytes));
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(sizeof(bytes) * 2, '0');
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        result[i * 2] = hex[bytes[i] >> 4];
        result[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    return result;
}

std::string sql_quote(const std::string& value) {
    std::string quoted;
    quoted.reserve(value.size() + 4);
    for (char ch : value) {
        if (ch == '\'') quoted += '\'';
        quoted += ch;
    }
    return quoted;
}

std::string canonical_manifest_rows(duckdb::Connection& connection) {
    auto result = connection.Query(
        "SELECT f.path, f.hash, COALESCE(string_agg(s.kind || ':' || s.name || ':' || "
        "COALESCE(s.signature, ''), '|' ORDER BY s.kind, s.name, s.signature), '') "
        "FROM files f LEFT JOIN symbols s ON s.file_id = f.id "
        "GROUP BY f.path, f.hash ORDER BY f.path");
    require_ok(result, "compute index manifest");
    std::string canonical;
    for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
        canonical += result->GetValue(0, row).ToString();
        canonical += '\0';
        canonical += result->GetValue(1, row).ToString();
        canonical += '\0';
        canonical += result->GetValue(2, row).ToString();
        canonical += '\n';
    }
    // Embeddings are derived index state too. Include their deterministic serialized values so an
    // embedding/model change advances manifest+epoch even when source and symbol text are stable.
    auto embeddings =
        connection.Query("SELECT f.path,s.kind,s.name,COALESCE(CAST(s.embedding AS VARCHAR),'') "
                         "FROM symbols s JOIN files f ON f.id=s.file_id "
                         "ORDER BY f.path,s.kind,s.name,s.id");
    require_ok(embeddings, "compute embedding manifest");
    for (duckdb::idx_t row = 0; row < embeddings->RowCount(); ++row) {
        canonical += "embedding\0";
        for (duckdb::idx_t column = 0; column < 4; ++column) {
            canonical += embeddings->GetValue(column, row).ToString();
            canonical += '\0';
        }
        canonical += '\n';
    }
    auto capability_evidence = connection.Query(
        "SELECT f.path,COALESCE(context.bounded_context,''),COALESCE(fingerprint.value,'') "
        "FROM files f LEFT JOIN capability_contexts context ON context.file_id=f.id "
        "LEFT JOIN capability_ast_fingerprints fingerprint ON fingerprint.file_id=f.id ORDER BY "
        "f.path");
    require_ok(capability_evidence, "compute capability evidence manifest");
    for (duckdb::idx_t row = 0; row < capability_evidence->RowCount(); ++row) {
        canonical += "capability_evidence\0";
        for (duckdb::idx_t column = 0; column < 3; ++column) {
            canonical += capability_evidence->GetValue(column, row).ToString();
            canonical += '\0';
        }
        canonical += '\n';
    }
    auto edges = connection.Query(
        "SELECT source.path, target.path, e.kind, COALESCE(origin.name, ''), "
        "COALESCE(destination.name, '') FROM edges e "
        "JOIN files source ON source.id=e.from_file JOIN files target ON target.id=e.to_file "
        "LEFT JOIN symbols origin ON origin.id=e.from_symbol "
        "LEFT JOIN symbols destination ON destination.id=e.to_symbol "
        "ORDER BY source.path,target.path,e.kind,origin.name,destination.name");
    require_ok(edges, "compute edge manifest");
    for (duckdb::idx_t row = 0; row < edges->RowCount(); ++row) {
        canonical += "edge\0";
        for (duckdb::idx_t column = 0; column < 5; ++column) {
            canonical += edges->GetValue(column, row).ToString();
            canonical += '\0';
        }
        canonical += '\n';
    }
    auto routes = connection.Query("SELECT method, path, handler_file, framework FROM routes "
                                   "ORDER BY method, path, handler_file, framework");
    require_ok(routes, "compute route manifest");
    for (duckdb::idx_t row = 0; row < routes->RowCount(); ++row) {
        canonical += "route\0";
        for (duckdb::idx_t column = 0; column < 4; ++column) {
            canonical += routes->GetValue(column, row).ToString();
            canonical += '\0';
        }
        canonical += '\n';
    }
    return canonical;
}

} // namespace

Transaction::Transaction(duckdb::Connection& connection) : connection_(connection) {
    auto result = connection_.Query("BEGIN TRANSACTION");
    require_ok(result, "begin index transaction");
}

Transaction::~Transaction() {
    if (!committed_) connection_.Query("ROLLBACK");
}

void Transaction::commit() {
    if (mutation_observed_ && !event_observed_)
        throw std::logic_error("index mutation cannot commit without an index event");
    trigger_journal_failpoint_for_testing("before_commit");
    auto result = connection_.Query("COMMIT");
    require_ok(result, "commit index transaction");
    committed_ = true;
    trigger_journal_failpoint_for_testing("after_commit");
}

void Transaction::mark_index_mutation() {
    mutation_observed_ = true;
}

void Transaction::mark_event_appended() {
    event_observed_ = true;
}

IndexIdentity index_identity(duckdb::Connection& connection) {
    auto result = connection.Query(
        "SELECT repository_id, index_stream_id, current_epoch, current_manifest, removed "
        "FROM index_metadata WHERE singleton = true");
    require_ok(result, "load index identity");
    if (result->RowCount() != 1) throw std::runtime_error("index identity is missing");
    return {result->GetValue(0, 0).ToString(), result->GetValue(1, 0).ToString(),
            result->GetValue(2, 0).ToString(), result->GetValue(3, 0).ToString(),
            result->GetValue<bool>(4, 0)};
}

std::string compute_manifest_hash(duckdb::Connection& connection) {
    return digest(canonical_manifest_rows(connection));
}

std::string compute_index_epoch(const IndexIdentity& identity, const std::string& manifest_hash) {
    return digest("axon/index-epoch/v1\n" + identity.repository_id + "\n" +
                  identity.index_stream_id + "\n" + manifest_hash);
}

static uint64_t append_index_event_impl(Transaction& transaction, duckdb::Connection& connection,
                                        const std::string& event_type,
                                        const std::vector<AffectedEntity>& affected,
                                        const std::string& manifest_hash,
                                        const std::optional<std::string>& source_ref,
                                        const IdentityChange* identity_change) {
    if (event_types.count(event_type) == 0)
        throw std::invalid_argument("unsupported index event type: " + event_type);
    if ((event_type == "RepositoryReidentified") != (identity_change != nullptr))
        throw std::invalid_argument("RepositoryReidentified requires typed identity_change");
    if (manifest_hash.size() < 16 || manifest_hash.size() > 128)
        throw std::invalid_argument("manifest_hash length is outside [16,128]");
    if (affected.size() > 10000)
        throw std::invalid_argument("index event affected set exceeds 10000 entities");
    if (source_ref && source_ref->size() > 512)
        throw std::invalid_argument("index event source_ref exceeds 512 bytes");
    bool has_delete = false;
    bool has_repository_delete = false;
    for (const auto& entity : affected) {
        if (entity_kinds.count(entity.kind) == 0)
            throw std::invalid_argument("unsupported affected entity kind: " + entity.kind);
        if (operations.count(entity.operation) == 0)
            throw std::invalid_argument("unsupported affected operation: " + entity.operation);
        if (entity.key.empty()) throw std::invalid_argument("affected entity key is empty");
        if (entity.key.size() > 4096)
            throw std::invalid_argument("affected entity key exceeds 4096 bytes");
        if (entity.digest && (entity.digest->size() < 16 || entity.digest->size() > 128))
            throw std::invalid_argument("affected entity digest length is outside [16,128]");
        has_delete = has_delete || entity.operation == "delete";
        has_repository_delete =
            has_repository_delete || (entity.kind == "repository" && entity.operation == "delete");
    }
    if (event_type == "IndexFilesDeleted" && !has_delete)
        throw std::invalid_argument("IndexFilesDeleted requires a delete operation");
    if (event_type == "RepositoryRemoved" && !has_repository_delete)
        throw std::invalid_argument("RepositoryRemoved requires a repository delete operation");

    const auto identity = index_identity(connection);
    if (identity.removed)
        throw std::logic_error("removed repository stream cannot publish new events");
    const std::string epoch = compute_index_epoch(identity, manifest_hash);
    auto sequence_result = connection.Query(
        "SELECT COALESCE(MAX(sequence), 0) + 1 FROM index_events WHERE index_stream_id = '" +
        sql_quote(identity.index_stream_id) + "'");
    require_ok(sequence_result, "allocate index event sequence");
    const uint64_t sequence = sequence_result->GetValue<uint64_t>(0, 0);
    const std::string event_id =
        digest(identity.repository_id + "\n" + identity.index_stream_id + "\n" +
               std::to_string(sequence) + "\n" + event_type + "\n" + epoch);

    nlohmann::json affected_json = nlohmann::json::array();
    for (const auto& entity : affected) {
        nlohmann::json item = {
            {"kind", entity.kind}, {"key", entity.key}, {"operation", entity.operation}};
        if (entity.digest) item["digest"] = *entity.digest;
        affected_json.push_back(std::move(item));
    }
    nlohmann::json payload = {{"affected", std::move(affected_json)}};
    if (identity_change) {
        payload["identity_change"] = {{"old_repository_id", identity_change->old_repository_id},
                                      {"new_repository_id", identity_change->new_repository_id},
                                      {"handoff_sequence", sequence},
                                      {"old_binding_id", identity_change->old_binding_id},
                                      {"new_binding_id", identity_change->new_binding_id},
                                      {"approval_reference", identity_change->approval_reference},
                                      {"reason", identity_change->reason}};
    }
    const std::string previous =
        identity.current_epoch.empty() ? "NULL" : "'" + sql_quote(identity.current_epoch) + "'";
    const std::string source = source_ref ? "'" + sql_quote(*source_ref) + "'" : "NULL";
    const std::string snapshot_manifest =
        event_type == "IndexSnapshotCompleted" ? "'" + sql_quote(manifest_hash) + "'" : "NULL";
    auto inserted = connection.Query(
        "INSERT INTO index_events(repository_id,index_stream_id,sequence,event_id,schema_version,"
        "event_type,index_epoch,previous_epoch,source_ref,manifest_hash,payload_json) VALUES ('" +
        sql_quote(identity.repository_id) + "','" + sql_quote(identity.index_stream_id) + "'," +
        std::to_string(sequence) + ",'" + event_id + "','axon/index-event/v1','" +
        sql_quote(event_type) + "','" + epoch + "'," + previous + "," + source + "," +
        snapshot_manifest + ",'" + sql_quote(payload.dump()) + "')");
    require_ok(inserted, "append index event");
    auto updated = connection.Query("UPDATE index_metadata SET current_epoch='" + epoch +
                                    "', current_manifest='" + sql_quote(manifest_hash) +
                                    "' WHERE singleton=true");
    require_ok(updated, "advance index epoch");
    transaction.mark_event_appended();
    trigger_journal_failpoint_for_testing("after_event");
    return sequence;
}

uint64_t append_index_event(Transaction& transaction, duckdb::Connection& connection,
                            const std::string& event_type,
                            const std::vector<AffectedEntity>& affected,
                            const std::string& manifest_hash,
                            const std::optional<std::string>& source_ref) {
    return append_index_event_impl(transaction, connection, event_type, affected, manifest_hash,
                                   source_ref, nullptr);
}

void upsert_tombstone(duckdb::Connection& connection, const AffectedEntity& entity,
                      uint64_t deleted_sequence, const std::string& deleted_epoch) {
    const auto identity = index_identity(connection);
    auto result = connection.Query(
        "INSERT INTO index_tombstones(repository_id,index_stream_id,entity_kind,entity_key,"
        "deleted_sequence,deleted_epoch,deleted_at) VALUES ('" +
        sql_quote(identity.repository_id) + "','" + sql_quote(identity.index_stream_id) + "','" +
        sql_quote(entity.kind) + "','" + sql_quote(entity.key) + "'," +
        std::to_string(deleted_sequence) + ",'" + sql_quote(deleted_epoch) +
        "',now()) "
        "ON CONFLICT(index_stream_id,entity_kind,entity_key) DO UPDATE SET "
        "deleted_sequence=excluded.deleted_sequence,deleted_epoch=excluded.deleted_epoch,"
        "deleted_at=excluded.deleted_at");
    require_ok(result, "upsert index tombstone");
}

void clear_tombstone(duckdb::Connection& connection, const AffectedEntity& entity) {
    const auto identity = index_identity(connection);
    auto result = connection.Query("DELETE FROM index_tombstones WHERE index_stream_id='" +
                                   sql_quote(identity.index_stream_id) + "' AND entity_kind='" +
                                   sql_quote(entity.kind) + "' AND entity_key='" +
                                   sql_quote(entity.key) + "'");
    require_ok(result, "clear resurrected tombstone");
}

uint64_t reidentify_repository(Transaction& transaction, duckdb::Connection& connection,
                               const IdentityChange& change) {
    const auto identity = index_identity(connection);
    if (identity.removed) throw std::logic_error("removed repository cannot be reidentified");
    if (!canonical_uuid(change.old_repository_id) || !canonical_uuid(change.new_repository_id) ||
        change.old_repository_id == change.new_repository_id)
        throw std::invalid_argument(
            "reidentification requires distinct canonical repository UUIDs");
    if (change.old_repository_id != identity.repository_id)
        throw std::invalid_argument("old_repository_id does not match persisted identity");
    if (change.old_binding_id.size() < 16 || change.old_binding_id.size() > 128 ||
        change.new_binding_id.size() < 16 || change.new_binding_id.size() > 128)
        throw std::invalid_argument("reidentification binding id length is outside [16,128]");
    if (change.approval_reference.empty() || change.approval_reference.size() > 512)
        throw std::invalid_argument("reidentification approval reference is invalid");
    if (identity_reasons.count(change.reason) == 0)
        throw std::invalid_argument("reidentification reason is invalid");

    transaction.mark_index_mutation();
    const std::vector<AffectedEntity> affected = {
        {"repository", change.old_repository_id, "delete", std::nullopt},
        {"repository", change.new_repository_id, "upsert", std::nullopt}};
    const uint64_t sequence = append_index_event_impl(
        transaction, connection, "RepositoryReidentified", affected,
        identity.current_manifest.empty() ? compute_manifest_hash(connection)
                                          : identity.current_manifest,
        change.approval_reference, &change);
    auto updated = connection.Query("UPDATE index_metadata SET repository_id='" +
                                    sql_quote(change.new_repository_id) +
                                    "', current_epoch='', removed=false WHERE singleton=true");
    require_ok(updated, "rebind repository identity");
    return sequence;
}

uint64_t remove_repository(Transaction& transaction, duckdb::Connection& connection,
                           const std::optional<std::string>& source_ref) {
    const auto identity = index_identity(connection);
    if (identity.removed) throw std::logic_error("repository stream is already removed");
    transaction.mark_index_mutation();
    const AffectedEntity removed = {"repository", identity.repository_id, "delete", std::nullopt};
    const std::string manifest = identity.current_manifest.empty()
                                     ? compute_manifest_hash(connection)
                                     : identity.current_manifest;
    const uint64_t sequence = append_index_event_impl(transaction, connection, "RepositoryRemoved",
                                                      {removed}, manifest, source_ref, nullptr);
    const std::string epoch = index_identity(connection).current_epoch;
    upsert_tombstone(connection, removed, sequence, epoch);
    auto updated = connection.Query("UPDATE index_metadata SET removed=true WHERE singleton=true");
    require_ok(updated, "mark repository removed");
    return sequence;
}

void set_journal_failpoint_for_testing(const std::string& failpoint) {
    std::lock_guard<std::mutex> lock(failpoint_mutex);
    active_failpoint = failpoint;
}

void clear_journal_failpoint_for_testing() {
    std::lock_guard<std::mutex> lock(failpoint_mutex);
    active_failpoint.clear();
}

void trigger_journal_failpoint_for_testing(const std::string& failpoint) {
    std::lock_guard<std::mutex> lock(failpoint_mutex);
    if (active_failpoint == failpoint)
        throw std::runtime_error("portfolio journal test failpoint: " + failpoint);
}

} // namespace axon::portfolio
