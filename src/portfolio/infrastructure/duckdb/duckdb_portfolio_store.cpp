#include "duckdb_portfolio_store.hpp"
#include "repository_reidentification_validation.hpp"

#include <blake3.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace axon::portfolio {
namespace {

constexpr std::size_t kMaxEventsPerBatch = 500;
constexpr std::size_t kMaxMutationsPerEvent = 10000;
constexpr std::size_t kMaxSnapshotEntities = 10000;
std::mutex store_mutex;
std::map<std::string, std::weak_ptr<duckdb::DuckDB>> databases_by_path;

std::shared_ptr<duckdb::DuckDB> database_for_path(const std::filesystem::path& path) {
    std::error_code error;
    const auto canonical =
        std::filesystem::weakly_canonical(std::filesystem::absolute(path, error), error);
    if (error) throw std::invalid_argument("portfolio database path cannot be resolved");
    const auto key = canonical.string();
    std::lock_guard<std::mutex> lock(store_mutex);
    if (auto existing = databases_by_path[key].lock()) return existing;
    auto created = std::make_shared<duckdb::DuckDB>(key);
    databases_by_path[key] = created;
    return created;
}

[[noreturn]] void fail(PortfolioStoreErrorCode code, const std::string& message) {
    throw PortfolioStoreError(code, message);
}

template <typename Result>
void ok(const std::unique_ptr<Result>& result, const char* operation) {
    if (!result || result->HasError())
        throw std::runtime_error(std::string(operation) + ": " +
                                 (result ? result->GetError() : "no DuckDB result"));
}

bool uuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[i])))
            return false;
    }
    return true;
}

void validate_stream(const RepositoryStreamKey& stream) {
    if (!uuid(stream.repository_id) || !uuid(stream.index_stream_id))
        fail(PortfolioStoreErrorCode::InvalidInput, "invalid repository stream identity");
}

void validate_mutation(const ProjectionMutation& mutation) {
    if (mutation.entity_kind.empty() || mutation.entity_kind.size() > 128 ||
        mutation.entity_key.empty() || mutation.entity_key.size() > 4096 ||
        (mutation.digest && (mutation.digest->size() < 16 || mutation.digest->size() > 128)))
        fail(PortfolioStoreErrorCode::InvalidInput, "invalid projection mutation");
}

class FingerprintBuilder {
public:
    explicit FingerprintBuilder(const char* domain) {
        blake3_hasher_init(&hasher_);
        add(domain);
    }

    void add(const std::string& value) {
        const auto length = std::to_string(value.size()) + ":";
        blake3_hasher_update(&hasher_, length.data(), length.size());
        blake3_hasher_update(&hasher_, value.data(), value.size());
    }

    std::string finish() {
        std::uint8_t bytes[BLAKE3_OUT_LEN];
        blake3_hasher_finalize(&hasher_, bytes, sizeof(bytes));
        static constexpr char hex[] = "0123456789abcdef";
        std::string result(sizeof(bytes) * 2, '0');
        for (std::size_t index = 0; index < sizeof(bytes); ++index) {
            result[index * 2] = hex[bytes[index] >> 4];
            result[index * 2 + 1] = hex[bytes[index] & 0x0f];
        }
        return result;
    }

private:
    blake3_hasher hasher_;
};

std::string fingerprint(const ProjectionEvent& event) {
    FingerprintBuilder builder("axon/portfolio-event-receipt/v1");
    builder.add(event.stream.repository_id);
    builder.add(event.stream.index_stream_id);
    builder.add(std::to_string(event.sequence));
    builder.add(event.event_id);
    builder.add(event.epoch);
    builder.add(event.manifest.value_or(""));
    for (const auto& mutation : event.mutations) {
        builder.add(mutation.entity_kind);
        builder.add(mutation.entity_key);
        builder.add(mutation.operation == ProjectionOperation::Upsert ? "U" : "D");
        builder.add(mutation.digest.value_or(""));
    }
    return builder.finish();
}

std::string fingerprint(const RepositoryReidentification& value) {
    FingerprintBuilder builder("axon/portfolio-reidentification-receipt/v1");
    builder.add(value.previous_stream.repository_id);
    builder.add(value.previous_stream.index_stream_id);
    builder.add(value.current_stream.repository_id);
    builder.add(value.current_stream.index_stream_id);
    builder.add(std::to_string(value.sequence));
    builder.add(value.event_id);
    builder.add(value.epoch);
    builder.add(value.manifest.value_or(""));
    builder.add(value.old_binding_id);
    builder.add(value.new_binding_id);
    builder.add(value.approval_reference);
    builder.add(value.reason);
    return builder.finish();
}

void validate_event(const ProjectionEvent& event, const RepositoryStreamKey& stream,
                    std::uint64_t sequence) {
    if (event.stream != stream || event.sequence != sequence)
        fail(PortfolioStoreErrorCode::CursorConflict, "event sequence/stream mismatch");
    if (event.event_id.size() < 16 || event.event_id.size() > 128 || event.epoch.size() < 16 ||
        event.epoch.size() > 128 ||
        (event.manifest && (event.manifest->size() < 16 || event.manifest->size() > 128)) ||
        event.mutations.size() > kMaxMutationsPerEvent)
        fail(PortfolioStoreErrorCode::InvalidInput, "invalid projection event bounds");
    for (const auto& mutation : event.mutations)
        validate_mutation(mutation);
}

void validate_reidentification(const RepositoryReidentification& value,
                               std::uint64_t expected_cursor) {
    const auto validation = duckdb_detail::validate_reidentification(value, expected_cursor);
    if (!validation) {
        fail(validation.error == duckdb_detail::ReidentificationValidationError::CursorConflict
                 ? PortfolioStoreErrorCode::CursorConflict
                 : PortfolioStoreErrorCode::InvalidInput,
             validation.message);
    }
}

struct Transaction {
    duckdb::Connection& connection;
    bool committed = false;
    explicit Transaction(duckdb::Connection& value) : connection(value) {
        ok(connection.Query("BEGIN TRANSACTION"), "begin portfolio transaction");
    }
    ~Transaction() {
        if (!committed) connection.Query("ROLLBACK");
    }
    void commit() {
        ok(connection.Query("COMMIT"), "commit portfolio transaction");
        committed = true;
    }
};

} // namespace

DuckdbPortfolioStore::DuckdbPortfolioStore()
    : database_(std::make_shared<duckdb::DuckDB>(nullptr)),
      connection_(std::make_unique<duckdb::Connection>(*database_)) {
    migrate();
}

DuckdbPortfolioStore::DuckdbPortfolioStore(const std::filesystem::path& path)
    : database_(database_for_path(path)),
      connection_(std::make_unique<duckdb::Connection>(*database_)) {
    migrate();
}

void DuckdbPortfolioStore::migrate() {
    std::lock_guard<std::mutex> lock(store_mutex);
    ok(connection_->Query("CREATE TABLE IF NOT EXISTS portfolio_schema_migrations("
                          "version INTEGER PRIMARY KEY, checksum VARCHAR NOT NULL)"),
       "schema ledger");
    auto migration =
        connection_->Query("SELECT checksum FROM portfolio_schema_migrations WHERE version=1");
    ok(migration, "read portfolio schema migration");
    if (migration->RowCount() > 1 ||
        (migration->RowCount() == 1 &&
         migration->GetValue(0, 0).ToString() != "axon/portfolio-duckdb/v1"))
        fail(PortfolioStoreErrorCode::IdempotencyConflict,
             "portfolio schema migration checksum mismatch");
    if (migration->RowCount() == 0)
        ok(connection_->Query("INSERT INTO portfolio_schema_migrations VALUES "
                              "(1,'axon/portfolio-duckdb/v1')"),
           "record portfolio schema migration");
    ok(connection_->Query(
           "CREATE TABLE IF NOT EXISTS portfolio_streams("
           "repository_id VARCHAR NOT NULL,index_stream_id VARCHAR NOT NULL,"
           "cursor UBIGINT NOT NULL,epoch VARCHAR NOT NULL,manifest VARCHAR NOT NULL,"
           "stale BOOLEAN NOT NULL,removed BOOLEAN NOT NULL,"
           "PRIMARY KEY(repository_id,index_stream_id),UNIQUE(index_stream_id))"),
       "stream schema");
    ok(connection_->Query("CREATE TABLE IF NOT EXISTS portfolio_entities("
                          "repository_id VARCHAR NOT NULL,index_stream_id VARCHAR NOT NULL,"
                          "entity_kind VARCHAR NOT NULL,entity_key VARCHAR NOT NULL,digest VARCHAR,"
                          "PRIMARY KEY(repository_id,index_stream_id,entity_kind,entity_key))"),
       "entity schema");
    ok(connection_->Query("CREATE TABLE IF NOT EXISTS portfolio_events("
                          "event_id VARCHAR PRIMARY KEY,repository_id VARCHAR NOT NULL,"
                          "index_stream_id VARCHAR NOT NULL,sequence UBIGINT NOT NULL,"
                          "fingerprint VARCHAR NOT NULL,UNIQUE(index_stream_id,sequence))"),
       "event schema");
    ok(connection_->Query("CREATE TABLE IF NOT EXISTS portfolio_reidentifications("
                          "event_id VARCHAR PRIMARY KEY,index_stream_id VARCHAR NOT NULL,"
                          "sequence UBIGINT NOT NULL,previous_repository_id VARCHAR NOT NULL,"
                          "current_repository_id VARCHAR NOT NULL,fingerprint VARCHAR NOT NULL,"
                          "UNIQUE(index_stream_id,sequence))"),
       "reidentification schema");
}

ProviderCapabilities DuckdbPortfolioStore::capabilities() const {
    return {ProviderRole::PortfolioStore,
            {ProviderCapability::AtomicApply, ProviderCapability::ReplaceRepositoryStream,
             ProviderCapability::ReidentifyRepositoryStream,
             ProviderCapability::ValidateMaintenance},
            kMaxEventsPerBatch,
            kMaxEventsPerBatch,
            kMaxMutationsPerEvent,
            kMaxSnapshotEntities};
}
ProviderHealth DuckdbPortfolioStore::health() const {
    try {
        std::lock_guard<std::mutex> lock(store_mutex);
        ok(connection_->Query("SELECT 1"), "health");
        return {ProviderHealthStatus::Healthy, "duckdb ready"};
    } catch (const std::exception& error) {
        return {ProviderHealthStatus::Unavailable, error.what()};
    }
}
std::string DuckdbPortfolioStore::schema_version() const {
    return "axon/portfolio-store/v1";
}
std::string DuckdbPortfolioStore::protocol_version() const {
    return "axon/portfolio-provider/v1";
}

CursorEpochManifest DuckdbPortfolioStore::state_unlocked(const RepositoryStreamKey& stream) const {
    auto statement =
        connection_->Prepare("SELECT cursor,epoch,manifest,stale,removed FROM portfolio_streams "
                             "WHERE repository_id=$1 AND index_stream_id=$2");
    auto result = statement->Execute(stream.repository_id, stream.index_stream_id);
    ok(result, "read stream state");
    auto rows = result->Fetch();
    if (!rows || rows->size() == 0) return {};
    return {true,
            rows->GetValue(0, 0).GetValue<std::uint64_t>(),
            rows->GetValue(1, 0).ToString(),
            rows->GetValue(2, 0).ToString(),
            rows->GetValue(3, 0).GetValue<bool>(),
            rows->GetValue(4, 0).GetValue<bool>()};
}

std::pair<RepositoryStreamKey, CursorEpochManifest>
DuckdbPortfolioStore::physical_state_unlocked(const std::string& index_stream_id) const {
    auto statement = connection_->Prepare(
        "SELECT repository_id,cursor,epoch,manifest,stale,removed FROM portfolio_streams "
        "WHERE index_stream_id=$1 LIMIT 2");
    auto result = statement->Execute(index_stream_id);
    ok(result, "read physical stream state");
    auto rows = result->Fetch();
    if (!rows || rows->size() == 0) return {};
    if (rows->size() != 1)
        fail(PortfolioStoreErrorCode::IdempotencyConflict,
             "physical index stream has multiple logical bindings");
    return {{rows->GetValue(0, 0).ToString(), index_stream_id},
            {true, rows->GetValue(1, 0).GetValue<std::uint64_t>(), rows->GetValue(2, 0).ToString(),
             rows->GetValue(3, 0).ToString(), rows->GetValue(4, 0).GetValue<bool>(),
             rows->GetValue(5, 0).GetValue<bool>()}};
}

CursorEpochManifest DuckdbPortfolioStore::stream_state(const RepositoryStreamKey& stream) const {
    validate_stream(stream);
    std::lock_guard<std::mutex> lock(store_mutex);
    return state_unlocked(stream);
}

ApplyResult DuckdbPortfolioStore::apply(const RepositoryStreamKey& stream,
                                        std::uint64_t expected_cursor,
                                        const std::vector<ProjectionEvent>& events) {
    validate_stream(stream);
    if (events.empty() || events.size() > kMaxEventsPerBatch)
        fail(PortfolioStoreErrorCode::InvalidInput, "invalid event batch size");
    std::uint64_t sequence = expected_cursor;
    for (const auto& event : events) {
        if (sequence == std::numeric_limits<std::uint64_t>::max())
            fail(PortfolioStoreErrorCode::CursorConflict, "sequence exhausted");
        validate_event(event, stream, ++sequence);
    }
    std::lock_guard<std::mutex> lock(store_mutex);
    Transaction transaction(*connection_);
    auto state = state_unlocked(stream);
    bool replay = true;
    auto receipt = connection_->Prepare("SELECT repository_id,index_stream_id,sequence,fingerprint "
                                        "FROM portfolio_events WHERE event_id=$1");
    for (const auto& event : events) {
        auto found = receipt->Execute(event.event_id);
        ok(found, "read event receipt");
        auto row = found->Fetch();
        if (!row || row->size() != 1 || row->GetValue(0, 0).ToString() != stream.repository_id ||
            row->GetValue(1, 0).ToString() != stream.index_stream_id ||
            row->GetValue(2, 0).GetValue<std::uint64_t>() != event.sequence ||
            row->GetValue(3, 0).ToString() != fingerprint(event)) {
            replay = false;
            break;
        }
    }
    if (replay) {
        if (!state.exists) state = physical_state_unlocked(stream.index_stream_id).second;
        if (!state.exists)
            fail(PortfolioStoreErrorCode::IdempotencyConflict,
                 "event receipt has no matching physical stream partition");
        transaction.commit();
        return {ApplyDisposition::Duplicate, state, 0};
    }
    if (!state.exists && physical_state_unlocked(stream.index_stream_id).second.exists)
        fail(PortfolioStoreErrorCode::IdempotencyConflict,
             "physical index stream is bound to another repository identity");
    if (state.cursor != expected_cursor)
        fail(PortfolioStoreErrorCode::CursorConflict, "portfolio stream cursor conflict");

    auto insert_event =
        connection_->Prepare("INSERT INTO portfolio_events VALUES ($1,$2,$3,$4,$5)");
    auto handoff_receipt = connection_->Prepare(
        "SELECT event_id FROM portfolio_reidentifications WHERE event_id=$1 OR "
        "(index_stream_id=$2 AND sequence=$3)");
    auto erase = connection_->Prepare("DELETE FROM portfolio_entities WHERE repository_id=$1 AND "
                                      "index_stream_id=$2 AND entity_kind=$3 AND entity_key=$4");
    auto upsert = connection_->Prepare("INSERT INTO portfolio_entities VALUES ($1,$2,$3,$4,$5) "
                                       "ON CONFLICT DO UPDATE SET digest=excluded.digest");
    std::size_t changed = 0;
    for (const auto& event : events) {
        auto handoff =
            handoff_receipt->Execute(event.event_id, stream.index_stream_id, event.sequence);
        ok(handoff, "read reidentification receipt");
        auto handoff_row = handoff->Fetch();
        if (handoff_row && handoff_row->size() != 0)
            fail(PortfolioStoreErrorCode::IdempotencyConflict,
                 "event_id is already bound to a reidentification");
        auto inserted =
            insert_event->Execute(event.event_id, stream.repository_id, stream.index_stream_id,
                                  event.sequence, fingerprint(event));
        if (inserted->HasError())
            fail(PortfolioStoreErrorCode::IdempotencyConflict, inserted->GetError());
        for (const auto& mutation : event.mutations) {
            std::unique_ptr<duckdb::QueryResult> result;
            if (mutation.operation == ProjectionOperation::Delete)
                result = erase->Execute(stream.repository_id, stream.index_stream_id,
                                        mutation.entity_kind, mutation.entity_key);
            else
                result = upsert->Execute(stream.repository_id, stream.index_stream_id,
                                         mutation.entity_kind, mutation.entity_key,
                                         mutation.digest.value_or(""));
            ok(result, "apply projection mutation");
            ++changed;
            if (mutation.entity_kind == "repository") {
                state.removed = mutation.operation == ProjectionOperation::Delete;
            }
        }
        state = {true,  event.sequence, event.epoch, event.manifest.value_or(state.manifest),
                 false, state.removed};
    }
    auto save = connection_->Prepare(
        "INSERT INTO portfolio_streams VALUES ($1,$2,$3,$4,$5,$6,$7) "
        "ON CONFLICT(repository_id,index_stream_id) DO UPDATE SET "
        "cursor=excluded.cursor,epoch=excluded.epoch,"
        "manifest=excluded.manifest,stale=excluded.stale,removed=excluded.removed");
    ok(save->Execute(stream.repository_id, stream.index_stream_id, state.cursor, state.epoch,
                     state.manifest, state.stale, state.removed),
       "save stream cursor");
    transaction.commit();
    return {ApplyDisposition::Applied, state, changed};
}

StreamProjection DuckdbPortfolioStore::inspect_repository_stream(const RepositoryStreamKey& stream,
                                                                 std::size_t max_entities) const {
    validate_stream(stream);
    if (max_entities == 0 || max_entities > 10000)
        fail(PortfolioStoreErrorCode::InvalidInput, "invalid inspection limit");
    std::lock_guard<std::mutex> lock(store_mutex);
    StreamProjection projection;
    projection.state = state_unlocked(stream);
    auto statement =
        connection_->Prepare("SELECT entity_kind,entity_key,digest FROM portfolio_entities "
                             "WHERE repository_id=$1 AND index_stream_id=$2 "
                             "ORDER BY entity_kind,entity_key LIMIT $3");
    auto result = statement->Execute(stream.repository_id, stream.index_stream_id,
                                     static_cast<std::int64_t>(max_entities + 1));
    ok(result, "inspect stream entities");
    while (auto rows = result->Fetch()) {
        for (duckdb::idx_t row = 0; row < rows->size(); ++row) {
            if (projection.entities.size() == max_entities) {
                projection.truncated = true;
                return projection;
            }
            const auto digest = rows->GetValue(2, row).ToString();
            projection.entities.push_back(
                {rows->GetValue(0, row).ToString(), rows->GetValue(1, row).ToString(),
                 ProjectionOperation::Upsert,
                 digest.empty() ? std::nullopt : std::optional<std::string>(digest)});
        }
    }
    return projection;
}

ReplaceResult DuckdbPortfolioStore::replace_repository_stream(const RepositorySnapshot& snapshot,
                                                              std::uint64_t expected_cursor) {
    validate_stream(snapshot.stream);
    if (snapshot.cursor == 0 || snapshot.cursor < expected_cursor || snapshot.epoch.size() < 16 ||
        snapshot.epoch.size() > 128 || snapshot.manifest.size() < 16 ||
        snapshot.manifest.size() > 128 || snapshot.entities.size() > kMaxSnapshotEntities)
        fail(PortfolioStoreErrorCode::InvalidInput, "invalid repository snapshot");
    std::map<std::pair<std::string, std::string>, ProjectionMutation> entities;
    for (const auto& entity : snapshot.entities) {
        validate_mutation(entity);
        if (entity.operation != ProjectionOperation::Upsert ||
            !entities.emplace(std::make_pair(entity.entity_kind, entity.entity_key), entity).second)
            fail(PortfolioStoreErrorCode::InvalidInput,
                 "invalid duplicate/deleted snapshot entity");
    }
    std::lock_guard<std::mutex> lock(store_mutex);
    Transaction transaction(*connection_);
    auto current = state_unlocked(snapshot.stream);
    const auto physical = physical_state_unlocked(snapshot.stream.index_stream_id);
    std::map<std::pair<std::string, std::string>, ProjectionMutation> current_entities;
    auto inspect =
        connection_->Prepare("SELECT entity_kind,entity_key,digest FROM portfolio_entities "
                             "WHERE repository_id=$1 AND index_stream_id=$2");
    auto rows = inspect->Execute(snapshot.stream.repository_id, snapshot.stream.index_stream_id);
    ok(rows, "read current partition");
    while (auto chunk = rows->Fetch()) {
        for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
            const auto digest = chunk->GetValue(2, row).ToString();
            ProjectionMutation entity = {
                chunk->GetValue(0, row).ToString(), chunk->GetValue(1, row).ToString(),
                ProjectionOperation::Upsert,
                digest.empty() ? std::nullopt : std::optional<std::string>(digest)};
            current_entities.emplace(std::make_pair(entity.entity_kind, entity.entity_key), entity);
        }
    }
    const CursorEpochManifest replacement = {
        true, snapshot.cursor, snapshot.epoch, snapshot.manifest, snapshot.stale, snapshot.removed};
    if (current == replacement && current_entities == entities) {
        transaction.commit();
        return {ApplyDisposition::Duplicate, current, 0};
    }
    if (!current.exists && physical.second.exists)
        fail(PortfolioStoreErrorCode::IdempotencyConflict,
             "physical index stream is bound to another repository identity");
    if (current.cursor != expected_cursor)
        fail(PortfolioStoreErrorCode::CursorConflict, "portfolio stream cursor conflict");
    auto erase = connection_->Prepare(
        "DELETE FROM portfolio_entities WHERE repository_id=$1 AND index_stream_id=$2");
    ok(erase->Execute(snapshot.stream.repository_id, snapshot.stream.index_stream_id),
       "replace partition");
    auto insert = connection_->Prepare("INSERT INTO portfolio_entities VALUES ($1,$2,$3,$4,$5)");
    for (const auto& [key, entity] : entities)
        ok(insert->Execute(snapshot.stream.repository_id, snapshot.stream.index_stream_id,
                           key.first, key.second, entity.digest.value_or("")),
           "insert snapshot entity");
    auto save = connection_->Prepare(
        "INSERT INTO portfolio_streams VALUES ($1,$2,$3,$4,$5,$6,$7) "
        "ON CONFLICT(repository_id,index_stream_id) DO UPDATE SET "
        "cursor=excluded.cursor,epoch=excluded.epoch,"
        "manifest=excluded.manifest,stale=excluded.stale,removed=excluded.removed");
    ok(save->Execute(snapshot.stream.repository_id, snapshot.stream.index_stream_id,
                     snapshot.cursor, snapshot.epoch, snapshot.manifest, snapshot.stale,
                     snapshot.removed),
       "replace stream state");
    transaction.commit();
    return {ApplyDisposition::Applied, replacement, entities.size()};
}

ApplyResult
DuckdbPortfolioStore::reidentify_repository_stream(const RepositoryReidentification& value,
                                                   std::uint64_t expected_cursor) {
    validate_reidentification(value, expected_cursor);
    std::lock_guard<std::mutex> lock(store_mutex);
    Transaction transaction(*connection_);
    auto receipt =
        connection_->Prepare("SELECT fingerprint FROM portfolio_events WHERE event_id=$1 OR "
                             "(index_stream_id=$2 AND sequence=$3)");
    auto receipt_result =
        receipt->Execute(value.event_id, value.previous_stream.index_stream_id, value.sequence);
    ok(receipt_result, "read global receipt for repository reidentification");
    auto receipt_row = receipt_result->Fetch();
    if (receipt_row && receipt_row->size() != 0) {
        if (receipt_row->GetValue(0, 0).ToString() != fingerprint(value))
            fail(PortfolioStoreErrorCode::IdempotencyConflict,
                 "reidentification identity or sequence is already bound");
        auto detail = connection_->Prepare(
            "SELECT fingerprint FROM portfolio_reidentifications WHERE event_id=$1");
        auto detail_result = detail->Execute(value.event_id);
        ok(detail_result, "read repository reidentification detail");
        auto detail_row = detail_result->Fetch();
        if (!detail_row || detail_row->size() != 1 ||
            detail_row->GetValue(0, 0).ToString() != fingerprint(value))
            fail(PortfolioStoreErrorCode::IdempotencyConflict,
                 "global reidentification receipt has no matching audit detail");
        const auto current = physical_state_unlocked(value.current_stream.index_stream_id).second;
        if (!current.exists || current.cursor < value.sequence)
            fail(PortfolioStoreErrorCode::IdempotencyConflict,
                 "reidentification receipt has no matching current partition");
        transaction.commit();
        return {ApplyDisposition::Duplicate, current, 0};
    }
    auto orphan_detail = connection_->Prepare(
        "SELECT event_id FROM portfolio_reidentifications WHERE event_id=$1 OR "
        "(index_stream_id=$2 AND sequence=$3)");
    auto orphan_result = orphan_detail->Execute(
        value.event_id, value.previous_stream.index_stream_id, value.sequence);
    ok(orphan_result, "inspect orphan reidentification detail");
    auto orphan_row = orphan_result->Fetch();
    if (orphan_row && orphan_row->size() != 0)
        fail(PortfolioStoreErrorCode::IdempotencyConflict,
             "reidentification audit detail has no global receipt");

    const auto previous = state_unlocked(value.previous_stream);
    if (!previous.exists || previous.cursor != expected_cursor)
        fail(PortfolioStoreErrorCode::CursorConflict, "reidentification previous cursor conflict");
    if (previous.removed)
        fail(PortfolioStoreErrorCode::InvalidInput,
             "removed repository stream cannot be reidentified");
    const auto physical = physical_state_unlocked(value.previous_stream.index_stream_id);
    if (!physical.second.exists || physical.first != value.previous_stream)
        fail(PortfolioStoreErrorCode::IdempotencyConflict,
             "physical index stream binding conflicts with reidentification source");

    auto insert_global_receipt =
        connection_->Prepare("INSERT INTO portfolio_events VALUES($1,$2,$3,$4,$5)");
    ok(insert_global_receipt->Execute(value.event_id, value.previous_stream.repository_id,
                                      value.previous_stream.index_stream_id, value.sequence,
                                      fingerprint(value)),
       "reserve global repository reidentification receipt");
    auto insert_receipt =
        connection_->Prepare("INSERT INTO portfolio_reidentifications VALUES($1,$2,$3,$4,$5,$6)");
    ok(insert_receipt->Execute(value.event_id, value.previous_stream.index_stream_id,
                               value.sequence, value.previous_stream.repository_id,
                               value.current_stream.repository_id, fingerprint(value)),
       "insert repository reidentification receipt");
    auto transfer_entities = connection_->Prepare("UPDATE portfolio_entities SET repository_id=$1 "
                                                  "WHERE repository_id=$2 AND index_stream_id=$3");
    ok(transfer_entities->Execute(value.current_stream.repository_id,
                                  value.previous_stream.repository_id,
                                  value.previous_stream.index_stream_id),
       "transfer repository projection entities");
    auto erase_old_identity = connection_->Prepare(
        "DELETE FROM portfolio_entities WHERE repository_id=$1 AND index_stream_id=$2 AND "
        "entity_kind='repository' AND entity_key=$3");
    ok(erase_old_identity->Execute(value.current_stream.repository_id,
                                   value.current_stream.index_stream_id,
                                   value.previous_stream.repository_id),
       "retire previous repository identity entity");
    auto upsert_identity =
        connection_->Prepare("INSERT INTO portfolio_entities VALUES($1,$2,'repository',$1,'') "
                             "ON CONFLICT DO UPDATE SET digest=excluded.digest");
    ok(upsert_identity->Execute(value.current_stream.repository_id,
                                value.current_stream.index_stream_id),
       "project current repository identity entity");
    auto transfer_stream = connection_->Prepare(
        "UPDATE portfolio_streams SET repository_id=$1,cursor=$2,epoch=$3,manifest=$4,"
        "stale=false,removed=false WHERE repository_id=$5 AND index_stream_id=$6");
    const auto manifest = value.manifest.value_or(previous.manifest);
    ok(transfer_stream->Execute(value.current_stream.repository_id, value.sequence, value.epoch,
                                manifest, value.previous_stream.repository_id,
                                value.previous_stream.index_stream_id),
       "transfer repository stream partition");
    transaction.commit();
    return {
        ApplyDisposition::Applied, {true, value.sequence, value.epoch, manifest, false, false}, 2};
}

MaintenanceResult DuckdbPortfolioStore::maintenance(MaintenanceKind kind) {
    if (kind != MaintenanceKind::Validate)
        fail(PortfolioStoreErrorCode::UnsupportedCapability, "maintenance capability unsupported");
    std::lock_guard<std::mutex> lock(store_mutex);
    auto result = connection_->Query("SELECT COUNT(*) FROM portfolio_streams WHERE cursor=0");
    ok(result, "validate portfolio store");
    if (result->GetValue<std::int64_t>(0, 0) != 0)
        fail(PortfolioStoreErrorCode::IdempotencyConflict, "invalid zero cursor state");
    result = connection_->Query(
        "SELECT COUNT(*) FROM portfolio_reidentifications r LEFT JOIN portfolio_events e "
        "ON e.event_id=r.event_id WHERE e.event_id IS NULL OR e.fingerprint<>r.fingerprint");
    ok(result, "validate reidentification audit receipts");
    if (result->GetValue<std::int64_t>(0, 0) != 0)
        fail(PortfolioStoreErrorCode::IdempotencyConflict,
             "orphan or divergent reidentification audit receipt");
    return {kind, true, "duckdb projection is internally consistent"};
}

} // namespace axon::portfolio
