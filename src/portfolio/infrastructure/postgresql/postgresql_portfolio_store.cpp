#include "postgresql_portfolio_store.hpp"

#include <blake3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace axon::portfolio {
namespace {

constexpr std::size_t kMaxEvents = 500;
constexpr std::size_t kMaxMutations = 10000;
constexpr std::size_t kMaxSnapshot = 10000;

class Result {
public:
    explicit Result(PGresult* value = nullptr) : value_(value) {}
    ~Result() {
        if (value_) PQclear(value_);
    }
    Result(Result&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Result(const Result&) = delete;
    PGresult* get() const { return value_; }

private:
    PGresult* value_;
};

class SqlError : public std::runtime_error {
public:
    SqlError(std::string state, const char* operation)
        : std::runtime_error(std::string(operation) + " failed"), state_(std::move(state)) {}
    const std::string& state() const { return state_; }

private:
    std::string state_;
};

Result checked(PGresult* raw, const char* operation, ExecStatusType expected) {
    Result result(raw);
    if (!raw || PQresultStatus(raw) != expected) {
        const auto* state = raw ? PQresultErrorField(raw, PG_DIAG_SQLSTATE) : nullptr;
        throw SqlError(state ? state : "", operation);
    }
    return result;
}

Result command(PGconn* connection, const std::string& sql, const char* operation) {
    return checked(PQexec(connection, sql.c_str()), operation, PGRES_COMMAND_OK);
}

Result execute(PGconn* connection, const std::string& sql,
               const std::vector<std::string>& parameters, const char* operation,
               ExecStatusType expected = PGRES_TUPLES_OK) {
    std::vector<const char*> values;
    values.reserve(parameters.size());
    for (const auto& parameter : parameters)
        values.push_back(parameter.c_str());
    return checked(PQexecParams(connection, sql.c_str(), static_cast<int>(values.size()), nullptr,
                                values.data(), nullptr, nullptr, 0),
                   operation, expected);
}

class Transaction {
public:
    explicit Transaction(PGconn* connection) : connection_(connection) {
        command(connection_, "BEGIN", "begin transaction");
    }
    ~Transaction() {
        if (!committed_) {
            auto* result = PQexec(connection_, "ROLLBACK");
            if (result) PQclear(result);
        }
    }
    void commit() {
        command(connection_, "COMMIT", "commit transaction");
        committed_ = true;
    }

private:
    PGconn* connection_;
    bool committed_ = false;
};

[[noreturn]] void fail(PortfolioStoreErrorCode code, const std::string& message) {
    throw PortfolioStoreError(code, message);
}

bool uuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[index])))
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

void validate_event(const ProjectionEvent& event, const RepositoryStreamKey& stream,
                    std::uint64_t sequence) {
    if (event.stream != stream || event.sequence != sequence)
        fail(PortfolioStoreErrorCode::CursorConflict, "event sequence/stream mismatch");
    if (event.event_id.size() < 16 || event.event_id.size() > 128 || event.epoch.size() < 16 ||
        event.epoch.size() > 128 ||
        (event.manifest && (event.manifest->size() < 16 || event.manifest->size() > 128)) ||
        event.mutations.size() > kMaxMutations)
        fail(PortfolioStoreErrorCode::InvalidInput, "invalid projection event bounds");
    for (const auto& mutation : event.mutations)
        validate_mutation(mutation);
}

void validate_reidentification(const RepositoryReidentification& value,
                               std::uint64_t expected_cursor) {
    static const std::unordered_set<std::string> reasons = {
        "contract-adopted", "collision-repaired", "repository-moved", "owner-approved"};
    validate_stream(value.previous_stream);
    validate_stream(value.current_stream);
    if (value.previous_stream.repository_id == value.current_stream.repository_id ||
        value.previous_stream.index_stream_id != value.current_stream.index_stream_id ||
        value.sequence == 0 || value.event_id.size() < 16 || value.event_id.size() > 128 ||
        value.epoch.size() < 16 || value.epoch.size() > 128 ||
        (value.manifest && (value.manifest->size() < 16 || value.manifest->size() > 128)) ||
        value.old_binding_id.size() < 16 || value.old_binding_id.size() > 128 ||
        value.new_binding_id.size() < 16 || value.new_binding_id.size() > 128 ||
        value.approval_reference.empty() || value.approval_reference.size() > 512 ||
        reasons.count(value.reason) == 0)
        fail(PortfolioStoreErrorCode::InvalidInput, "invalid repository reidentification bounds");
    if (expected_cursor == std::numeric_limits<std::uint64_t>::max() ||
        value.sequence != expected_cursor + 1)
        fail(PortfolioStoreErrorCode::CursorConflict,
             "repository reidentification sequence is not contiguous");
}

class Fingerprint {
public:
    explicit Fingerprint(const char* domain) {
        blake3_hasher_init(&hasher_);
        add(domain);
    }
    void add(const std::string& value) {
        const auto size = std::to_string(value.size()) + ":";
        blake3_hasher_update(&hasher_, size.data(), size.size());
        blake3_hasher_update(&hasher_, value.data(), value.size());
    }
    std::string finish() {
        std::uint8_t bytes[BLAKE3_OUT_LEN];
        blake3_hasher_finalize(&hasher_, bytes, sizeof(bytes));
        static constexpr char hex[] = "0123456789abcdef";
        std::string result(sizeof(bytes) * 2, '0');
        for (std::size_t index = 0; index < sizeof(bytes); ++index) {
            result[index * 2] = hex[bytes[index] >> 4];
            result[index * 2 + 1] = hex[bytes[index] & 15];
        }
        return result;
    }

private:
    blake3_hasher hasher_;
};

std::string fingerprint(const ProjectionEvent& event) {
    Fingerprint hash("axon/portfolio-event-receipt/v1");
    hash.add(event.stream.repository_id);
    hash.add(event.stream.index_stream_id);
    hash.add(std::to_string(event.sequence));
    hash.add(event.event_id);
    hash.add(event.epoch);
    hash.add(event.manifest.value_or(""));
    for (const auto& mutation : event.mutations) {
        hash.add(mutation.entity_kind);
        hash.add(mutation.entity_key);
        hash.add(mutation.operation == ProjectionOperation::Upsert ? "U" : "D");
        hash.add(mutation.digest.value_or(""));
    }
    return hash.finish();
}

std::string fingerprint(const RepositoryReidentification& value) {
    Fingerprint hash("axon/portfolio-reidentification-receipt/v1");
    hash.add(value.previous_stream.repository_id);
    hash.add(value.previous_stream.index_stream_id);
    hash.add(value.current_stream.repository_id);
    hash.add(value.current_stream.index_stream_id);
    hash.add(std::to_string(value.sequence));
    hash.add(value.event_id);
    hash.add(value.epoch);
    hash.add(value.manifest.value_or(""));
    hash.add(value.old_binding_id);
    hash.add(value.new_binding_id);
    hash.add(value.approval_reference);
    hash.add(value.reason);
    return hash.finish();
}

std::uint64_t number(PGresult* result, int row, int column) {
    return std::stoull(PQgetvalue(result, row, column));
}
bool boolean(PGresult* result, int row, int column) {
    return std::string(PQgetvalue(result, row, column)) == "t";
}
void lock_stream(PGconn* connection, const std::string& stream) {
    (void)execute(connection, "SELECT pg_advisory_xact_lock(hashtextextended($1,0))", {stream},
                  "lock physical stream");
}
void discard_notice(void*, const char*) {
}
void map_conflict(const SqlError& error) {
    if (error.state() == "23505" || error.state() == "23503")
        fail(PortfolioStoreErrorCode::IdempotencyConflict, "PostgreSQL projection conflict");
    throw error;
}

} // namespace

PostgresqlPortfolioStore::PostgresqlPortfolioStore(std::string connection_string,
                                                   std::string schema, bool drop_schema_on_destroy)
    : schema_(std::move(schema)), drop_schema_on_destroy_(drop_schema_on_destroy) {
    if (schema_.empty() || schema_.size() > 63 || schema_[0] < 'a' || schema_[0] > 'z' ||
        !std::all_of(schema_.begin(), schema_.end(),
                     [](unsigned char character) {
                         return std::islower(character) || std::isdigit(character) ||
                                character == '_';
                     }) ||
        schema_ == "public" || schema_ == "information_schema" || schema_.rfind("pg_", 0) == 0)
        throw std::invalid_argument("invalid PostgreSQL portfolio schema identifier");
    if (drop_schema_on_destroy_ && schema_.rfind("axon_test_", 0) != 0)
        throw std::invalid_argument(
            "destructive PostgreSQL teardown requires an axon_test_ namespace");
    connection_ = PQconnectdb(connection_string.c_str());
    if (!connection_ || PQstatus(connection_) != CONNECTION_OK) {
        if (connection_) PQfinish(connection_);
        connection_ = nullptr;
        throw std::runtime_error("PostgreSQL portfolio connection unavailable");
    }
    PQsetNoticeProcessor(connection_, discard_notice, nullptr);
    try {
        migrate();
    } catch (...) {
        PQfinish(connection_);
        connection_ = nullptr;
        throw;
    }
}

PostgresqlPortfolioStore::~PostgresqlPortfolioStore() {
    if (!connection_) return;
    if (drop_schema_on_destroy_ && schema_created_by_this_instance_) {
        auto* result =
            PQexec(connection_, ("DROP SCHEMA IF EXISTS " + schema_ + " CASCADE").c_str());
        if (result) PQclear(result);
    }
    PQfinish(connection_);
}

std::string PostgresqlPortfolioStore::table(const char* name) const {
    return schema_ + "." + name;
}

void PostgresqlPortfolioStore::migrate() {
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction transaction(connection_);
    lock_stream(connection_, "schema:" + schema_);
    auto schema_exists =
        execute(connection_, "SELECT EXISTS(SELECT 1 FROM pg_namespace WHERE nspname=$1)",
                {schema_}, "inspect schema ownership");
    schema_created_by_this_instance_ = !boolean(schema_exists.get(), 0, 0);
    command(connection_, "CREATE SCHEMA IF NOT EXISTS " + schema_, "create schema");
    command(connection_,
            "CREATE TABLE IF NOT EXISTS " + table("portfolio_schema_migrations") +
                "(version integer PRIMARY KEY,checksum text NOT NULL)",
            "create migration ledger");
    auto migrations = execute(connection_,
                              "SELECT version,checksum FROM " +
                                  table("portfolio_schema_migrations") + " ORDER BY version",
                              {}, "read migration ledger");
    bool has_v1 = false;
    bool has_v2 = false;
    for (int row = 0; row < PQntuples(migrations.get()); ++row) {
        const auto version = std::stoi(PQgetvalue(migrations.get(), row, 0));
        const std::string checksum = PQgetvalue(migrations.get(), row, 1);
        if (version == 1 && checksum == "axon/portfolio-postgresql/v1")
            has_v1 = true;
        else if (version == 2 && checksum == "axon/portfolio-postgresql-outbox/v2")
            has_v2 = true;
        else
            fail(PortfolioStoreErrorCode::IdempotencyConflict, "migration checksum mismatch");
    }
    command(connection_,
            "CREATE TABLE IF NOT EXISTS " + table("portfolio_streams") +
                "(repository_id text NOT NULL,index_stream_id text NOT NULL UNIQUE,cursor "
                "numeric(20,0) "
                "NOT NULL,epoch text NOT NULL,manifest text NOT NULL,stale boolean NOT "
                "NULL,removed boolean "
                "NOT NULL,PRIMARY KEY(repository_id,index_stream_id))",
            "create streams");
    command(
        connection_,
        "CREATE TABLE IF NOT EXISTS " + table("portfolio_entities") +
            "(repository_id text NOT NULL,index_stream_id text NOT NULL,entity_kind text NOT NULL,"
            "entity_key text NOT NULL,digest text,PRIMARY "
            "KEY(repository_id,index_stream_id,entity_kind,"
            "entity_key))",
        "create entities");
    command(
        connection_,
        "CREATE TABLE IF NOT EXISTS " + table("portfolio_events") +
            "(event_id text PRIMARY KEY,repository_id text NOT NULL,index_stream_id text NOT NULL,"
            "sequence numeric(20,0) NOT NULL,fingerprint text NOT "
            "NULL,UNIQUE(index_stream_id,sequence))",
        "create events");
    if (!has_v1)
        command(connection_,
                "INSERT INTO " + table("portfolio_schema_migrations") +
                    " VALUES(1,'axon/portfolio-postgresql/v1')",
                "record core migration");
    command(connection_,
            "CREATE TABLE IF NOT EXISTS " + table("portfolio_reidentifications") +
                "(event_id text PRIMARY KEY,index_stream_id text NOT NULL,sequence numeric(20,0) "
                "NOT NULL,"
                "previous_repository_id text NOT NULL,current_repository_id text NOT "
                "NULL,fingerprint text "
                "NOT NULL,UNIQUE(index_stream_id,sequence))",
            "create reidentifications");
    command(connection_,
            "CREATE TABLE IF NOT EXISTS " + table("portfolio_outbox") +
                "(event_id text PRIMARY KEY,event_kind text NOT NULL,repository_id text NOT NULL,"
                "index_stream_id text NOT NULL,sequence numeric(20,0) NOT NULL,published boolean "
                "NOT NULL "
                "DEFAULT false,created_at timestamptz NOT NULL DEFAULT clock_timestamp())",
            "create outbox");
    if (!has_v2)
        command(connection_,
                "INSERT INTO " + table("portfolio_schema_migrations") +
                    " VALUES(2,'axon/portfolio-postgresql-outbox/v2')",
                "record outbox migration");
    transaction.commit();
}

ProviderCapabilities PostgresqlPortfolioStore::capabilities() const {
    return {ProviderRole::PortfolioStore,
            {ProviderCapability::AtomicApply, ProviderCapability::ReplaceRepositoryStream,
             ProviderCapability::ReidentifyRepositoryStream,
             ProviderCapability::ValidateMaintenance},
            kMaxEvents,
            kMaxEvents,
            kMaxMutations,
            kMaxSnapshot};
}

ProviderHealth PostgresqlPortfolioStore::health() const {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        (void)execute(connection_, "SELECT 1", {}, "health");
        return {ProviderHealthStatus::Healthy, "postgresql ready"};
    } catch (const std::exception&) {
        return {ProviderHealthStatus::Unavailable, "postgresql unavailable"};
    }
}

std::string PostgresqlPortfolioStore::schema_version() const {
    return "axon/portfolio-store/v1";
}
std::string PostgresqlPortfolioStore::protocol_version() const {
    return "axon/portfolio-provider/v1";
}

CursorEpochManifest PostgresqlPortfolioStore::state_unlocked(const RepositoryStreamKey& stream,
                                                             bool lock_row) const {
    auto result = execute(
        connection_,
        "SELECT cursor,epoch,manifest,stale,removed FROM " + table("portfolio_streams") +
            " WHERE repository_id=$1 AND index_stream_id=$2" + (lock_row ? " FOR UPDATE" : ""),
        {stream.repository_id, stream.index_stream_id}, "read stream");
    if (PQntuples(result.get()) == 0) return {};
    return {true,
            number(result.get(), 0, 0),
            PQgetvalue(result.get(), 0, 1),
            PQgetvalue(result.get(), 0, 2),
            boolean(result.get(), 0, 3),
            boolean(result.get(), 0, 4)};
}

std::pair<RepositoryStreamKey, CursorEpochManifest>
PostgresqlPortfolioStore::physical_state_unlocked(const std::string& index_stream_id,
                                                  bool lock_row) const {
    auto result = execute(connection_,
                          "SELECT repository_id,cursor,epoch,manifest,stale,removed "
                          "FROM " +
                              table("portfolio_streams") + " WHERE index_stream_id=$1" +
                              (lock_row ? " FOR UPDATE" : ""),
                          {index_stream_id}, "read physical stream");
    if (PQntuples(result.get()) == 0) return {};
    if (PQntuples(result.get()) != 1)
        fail(PortfolioStoreErrorCode::IdempotencyConflict, "physical stream has multiple bindings");
    return {{PQgetvalue(result.get(), 0, 0), index_stream_id},
            {true, number(result.get(), 0, 1), PQgetvalue(result.get(), 0, 2),
             PQgetvalue(result.get(), 0, 3), boolean(result.get(), 0, 4),
             boolean(result.get(), 0, 5)}};
}

CursorEpochManifest
PostgresqlPortfolioStore::stream_state(const RepositoryStreamKey& stream) const {
    validate_stream(stream);
    std::lock_guard<std::mutex> lock(mutex_);
    return state_unlocked(stream);
}

ApplyResult PostgresqlPortfolioStore::apply(const RepositoryStreamKey& stream,
                                            std::uint64_t expected_cursor,
                                            const std::vector<ProjectionEvent>& events) {
    validate_stream(stream);
    if (events.empty() || events.size() > kMaxEvents)
        fail(PortfolioStoreErrorCode::InvalidInput, "invalid event batch size");
    auto sequence = expected_cursor;
    for (const auto& event : events) {
        if (sequence == std::numeric_limits<std::uint64_t>::max())
            fail(PortfolioStoreErrorCode::CursorConflict, "sequence exhausted");
        validate_event(event, stream, ++sequence);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction transaction(connection_);
    lock_stream(connection_, stream.index_stream_id);
    auto state = state_unlocked(stream, true);
    bool replay = true;
    for (const auto& event : events) {
        auto receipt = execute(connection_,
                               "SELECT repository_id,index_stream_id,sequence,"
                               "fingerprint FROM " +
                                   table("portfolio_events") + " WHERE event_id=$1",
                               {event.event_id}, "read receipt");
        if (PQntuples(receipt.get()) != 1 ||
            std::string(PQgetvalue(receipt.get(), 0, 0)) != stream.repository_id ||
            std::string(PQgetvalue(receipt.get(), 0, 1)) != stream.index_stream_id ||
            number(receipt.get(), 0, 2) != event.sequence ||
            std::string(PQgetvalue(receipt.get(), 0, 3)) != fingerprint(event)) {
            replay = false;
            break;
        }
    }
    if (replay) {
        if (!state.exists) state = physical_state_unlocked(stream.index_stream_id, true).second;
        if (!state.exists)
            fail(PortfolioStoreErrorCode::IdempotencyConflict, "receipt has no physical stream");
        transaction.commit();
        return {ApplyDisposition::Duplicate, state, 0};
    }
    if (!state.exists && physical_state_unlocked(stream.index_stream_id, true).second.exists)
        fail(PortfolioStoreErrorCode::IdempotencyConflict,
             "physical stream is bound to another repository");
    if (state.cursor != expected_cursor)
        fail(PortfolioStoreErrorCode::CursorConflict, "portfolio stream cursor conflict");

    std::size_t changed = 0;
    try {
        for (const auto& event : events) {
            execute(connection_,
                    "INSERT INTO " + table("portfolio_events") + " VALUES($1,$2,$3,$4,$5)",
                    {event.event_id, stream.repository_id, stream.index_stream_id,
                     std::to_string(event.sequence), fingerprint(event)},
                    "insert receipt", PGRES_COMMAND_OK);
            for (const auto& mutation : event.mutations) {
                if (mutation.operation == ProjectionOperation::Delete)
                    execute(
                        connection_,
                        "DELETE FROM " + table("portfolio_entities") +
                            " WHERE repository_id=$1 AND index_stream_id=$2 AND entity_kind=$3 AND "
                            "entity_key=$4",
                        {stream.repository_id, stream.index_stream_id, mutation.entity_kind,
                         mutation.entity_key},
                        "delete entity", PGRES_COMMAND_OK);
                else
                    execute(connection_,
                            "INSERT INTO " + table("portfolio_entities") +
                                " VALUES($1,$2,$3,$4,$5) ON CONFLICT(repository_id,index_stream_id,"
                                "entity_kind,entity_key) DO UPDATE SET digest=excluded.digest",
                            {stream.repository_id, stream.index_stream_id, mutation.entity_kind,
                             mutation.entity_key, mutation.digest.value_or("")},
                            "upsert entity", PGRES_COMMAND_OK);
                ++changed;
                if (mutation.entity_kind == "repository")
                    state.removed = mutation.operation == ProjectionOperation::Delete;
            }
            state = {true,  event.sequence, event.epoch, event.manifest.value_or(state.manifest),
                     false, state.removed};
            execute(connection_,
                    "INSERT INTO " + table("portfolio_outbox") +
                        "(event_id,event_kind,repository_id,index_stream_id,sequence) "
                        "VALUES($1,'projection',"
                        "$2,$3,$4)",
                    {event.event_id, stream.repository_id, stream.index_stream_id,
                     std::to_string(event.sequence)},
                    "insert outbox", PGRES_COMMAND_OK);
        }
        execute(connection_,
                "INSERT INTO " + table("portfolio_streams") +
                    " VALUES($1,$2,$3,$4,$5,$6,$7) ON CONFLICT(repository_id,index_stream_id) DO "
                    "UPDATE SET "
                    "cursor=excluded.cursor,epoch=excluded.epoch,manifest=excluded.manifest,"
                    "stale=excluded.stale,removed=excluded.removed",
                {stream.repository_id, stream.index_stream_id, std::to_string(state.cursor),
                 state.epoch, state.manifest, state.stale ? "true" : "false",
                 state.removed ? "true" : "false"},
                "save stream", PGRES_COMMAND_OK);
    } catch (const SqlError& error) {
        map_conflict(error);
    }
    transaction.commit();
    return {ApplyDisposition::Applied, state, changed};
}

StreamProjection
PostgresqlPortfolioStore::inspect_repository_stream(const RepositoryStreamKey& stream,
                                                    std::size_t max_entities) const {
    validate_stream(stream);
    if (max_entities == 0 || max_entities > kMaxSnapshot)
        fail(PortfolioStoreErrorCode::InvalidInput, "invalid inspection limit");
    std::lock_guard<std::mutex> lock(mutex_);
    StreamProjection projection;
    projection.state = state_unlocked(stream);
    auto rows =
        execute(connection_,
                "SELECT entity_kind,entity_key,digest FROM " + table("portfolio_entities") +
                    " WHERE repository_id=$1 AND index_stream_id=$2 ORDER BY "
                    "entity_kind,entity_key LIMIT $3",
                {stream.repository_id, stream.index_stream_id, std::to_string(max_entities + 1)},
                "inspect entities");
    for (int row = 0; row < PQntuples(rows.get()); ++row) {
        if (projection.entities.size() == max_entities) {
            projection.truncated = true;
            break;
        }
        const std::string digest = PQgetvalue(rows.get(), row, 2);
        projection.entities.push_back(
            {PQgetvalue(rows.get(), row, 0), PQgetvalue(rows.get(), row, 1),
             ProjectionOperation::Upsert,
             digest.empty() ? std::nullopt : std::optional<std::string>(digest)});
    }
    return projection;
}

ReplaceResult
PostgresqlPortfolioStore::replace_repository_stream(const RepositorySnapshot& snapshot,
                                                    std::uint64_t expected_cursor) {
    validate_stream(snapshot.stream);
    if (snapshot.cursor == 0 || snapshot.cursor < expected_cursor || snapshot.epoch.size() < 16 ||
        snapshot.epoch.size() > 128 || snapshot.manifest.size() < 16 ||
        snapshot.manifest.size() > 128 || snapshot.entities.size() > kMaxSnapshot)
        fail(PortfolioStoreErrorCode::InvalidInput, "invalid repository snapshot");
    std::map<std::pair<std::string, std::string>, ProjectionMutation> entities;
    for (const auto& entity : snapshot.entities) {
        validate_mutation(entity);
        if (entity.operation != ProjectionOperation::Upsert ||
            !entities.emplace(std::make_pair(entity.entity_kind, entity.entity_key), entity).second)
            fail(PortfolioStoreErrorCode::InvalidInput, "invalid snapshot entity");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction transaction(connection_);
    lock_stream(connection_, snapshot.stream.index_stream_id);
    const auto current = state_unlocked(snapshot.stream, true);
    if (!current.exists &&
        physical_state_unlocked(snapshot.stream.index_stream_id, true).second.exists)
        fail(PortfolioStoreErrorCode::IdempotencyConflict,
             "physical stream is bound to another repository");
    if (current.cursor != expected_cursor)
        fail(PortfolioStoreErrorCode::CursorConflict, "portfolio stream cursor conflict");
    auto existing =
        execute(connection_,
                "SELECT entity_kind,entity_key,digest FROM " + table("portfolio_entities") +
                    " WHERE repository_id=$1 AND index_stream_id=$2",
                {snapshot.stream.repository_id, snapshot.stream.index_stream_id}, "read partition");
    std::map<std::pair<std::string, std::string>, ProjectionMutation> old_entities;
    for (int row = 0; row < PQntuples(existing.get()); ++row) {
        const std::string digest = PQgetvalue(existing.get(), row, 2);
        ProjectionMutation entity{PQgetvalue(existing.get(), row, 0),
                                  PQgetvalue(existing.get(), row, 1), ProjectionOperation::Upsert,
                                  digest.empty() ? std::nullopt
                                                 : std::optional<std::string>(digest)};
        old_entities.emplace(std::make_pair(entity.entity_kind, entity.entity_key), entity);
    }
    const CursorEpochManifest replacement{
        true, snapshot.cursor, snapshot.epoch, snapshot.manifest, snapshot.stale, snapshot.removed};
    if (current == replacement && old_entities == entities) {
        transaction.commit();
        return {ApplyDisposition::Duplicate, current, 0};
    }
    execute(connection_,
            "DELETE FROM " + table("portfolio_entities") +
                " WHERE repository_id=$1 AND index_stream_id=$2",
            {snapshot.stream.repository_id, snapshot.stream.index_stream_id}, "clear partition",
            PGRES_COMMAND_OK);
    for (const auto& [key, entity] : entities)
        execute(connection_,
                "INSERT INTO " + table("portfolio_entities") + " VALUES($1,$2,$3,$4,$5)",
                {snapshot.stream.repository_id, snapshot.stream.index_stream_id, key.first,
                 key.second, entity.digest.value_or("")},
                "insert snapshot entity", PGRES_COMMAND_OK);
    execute(connection_,
            "INSERT INTO " + table("portfolio_streams") +
                " VALUES($1,$2,$3,$4,$5,$6,$7) ON CONFLICT(repository_id,index_stream_id) DO "
                "UPDATE SET "
                "cursor=excluded.cursor,epoch=excluded.epoch,manifest=excluded.manifest,"
                "stale=excluded.stale,removed=excluded.removed",
            {snapshot.stream.repository_id, snapshot.stream.index_stream_id,
             std::to_string(snapshot.cursor), snapshot.epoch, snapshot.manifest,
             snapshot.stale ? "true" : "false", snapshot.removed ? "true" : "false"},
            "replace stream", PGRES_COMMAND_OK);
    execute(connection_,
            "INSERT INTO " + table("portfolio_outbox") +
                "(event_id,event_kind,repository_id,index_stream_id,sequence) VALUES("
                "'replace-' || txid_current()::text,'replace',$1,$2,$3)",
            {snapshot.stream.repository_id, snapshot.stream.index_stream_id,
             std::to_string(snapshot.cursor)},
            "replace outbox", PGRES_COMMAND_OK);
    transaction.commit();
    return {ApplyDisposition::Applied, replacement, entities.size()};
}

ApplyResult
PostgresqlPortfolioStore::reidentify_repository_stream(const RepositoryReidentification& value,
                                                       std::uint64_t expected_cursor) {
    validate_reidentification(value, expected_cursor);
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction transaction(connection_);
    lock_stream(connection_, value.previous_stream.index_stream_id);
    auto receipt = execute(
        connection_,
        "SELECT fingerprint FROM " + table("portfolio_events") +
            " WHERE event_id=$1 OR (index_stream_id=$2 AND sequence=$3)",
        {value.event_id, value.previous_stream.index_stream_id, std::to_string(value.sequence)},
        "read handoff receipt");
    if (PQntuples(receipt.get()) != 0) {
        if (PQntuples(receipt.get()) != 1 ||
            std::string(PQgetvalue(receipt.get(), 0, 0)) != fingerprint(value))
            fail(PortfolioStoreErrorCode::IdempotencyConflict, "reidentification receipt conflict");
        auto detail = execute(connection_,
                              "SELECT fingerprint FROM " + table("portfolio_reidentifications") +
                                  " WHERE event_id=$1",
                              {value.event_id}, "read handoff detail");
        const auto current =
            physical_state_unlocked(value.current_stream.index_stream_id, true).second;
        if (PQntuples(detail.get()) != 1 ||
            std::string(PQgetvalue(detail.get(), 0, 0)) != fingerprint(value) || !current.exists ||
            current.cursor < value.sequence)
            fail(PortfolioStoreErrorCode::IdempotencyConflict,
                 "reidentification receipt has no current partition");
        transaction.commit();
        return {ApplyDisposition::Duplicate, current, 0};
    }
    const auto previous = state_unlocked(value.previous_stream, true);
    if (!previous.exists || previous.cursor != expected_cursor)
        fail(PortfolioStoreErrorCode::CursorConflict, "reidentification cursor conflict");
    if (previous.removed)
        fail(PortfolioStoreErrorCode::InvalidInput, "removed stream cannot be reidentified");
    try {
        execute(connection_, "INSERT INTO " + table("portfolio_events") + " VALUES($1,$2,$3,$4,$5)",
                {value.event_id, value.previous_stream.repository_id,
                 value.previous_stream.index_stream_id, std::to_string(value.sequence),
                 fingerprint(value)},
                "insert handoff receipt", PGRES_COMMAND_OK);
        execute(connection_,
                "INSERT INTO " + table("portfolio_reidentifications") +
                    " VALUES($1,$2,$3,$4,$5,$6)",
                {value.event_id, value.previous_stream.index_stream_id,
                 std::to_string(value.sequence), value.previous_stream.repository_id,
                 value.current_stream.repository_id, fingerprint(value)},
                "insert handoff detail", PGRES_COMMAND_OK);
        execute(connection_,
                "UPDATE " + table("portfolio_entities") +
                    " SET repository_id=$1 WHERE repository_id=$2 AND index_stream_id=$3",
                {value.current_stream.repository_id, value.previous_stream.repository_id,
                 value.previous_stream.index_stream_id},
                "transfer entities", PGRES_COMMAND_OK);
        execute(
            connection_,
            "DELETE FROM " + table("portfolio_entities") +
                " WHERE repository_id=$1 AND index_stream_id=$2 AND entity_kind='repository' AND "
                "entity_key=$3",
            {value.current_stream.repository_id, value.current_stream.index_stream_id,
             value.previous_stream.repository_id},
            "delete old identity", PGRES_COMMAND_OK);
        execute(connection_,
                "INSERT INTO " + table("portfolio_entities") +
                    " VALUES($1,$2,'repository',$1,'') ON CONFLICT(repository_id,index_stream_id,"
                    "entity_kind,entity_key) DO UPDATE SET digest=excluded.digest",
                {value.current_stream.repository_id, value.current_stream.index_stream_id},
                "upsert new identity", PGRES_COMMAND_OK);
        const auto manifest = value.manifest.value_or(previous.manifest);
        execute(
            connection_,
            "UPDATE " + table("portfolio_streams") +
                " SET repository_id=$1,cursor=$2,epoch=$3,manifest=$4,stale=false,removed=false "
                "WHERE repository_id=$5 AND index_stream_id=$6",
            {value.current_stream.repository_id, std::to_string(value.sequence), value.epoch,
             manifest, value.previous_stream.repository_id, value.previous_stream.index_stream_id},
            "transfer stream", PGRES_COMMAND_OK);
        execute(connection_,
                "INSERT INTO " + table("portfolio_outbox") +
                    "(event_id,event_kind,repository_id,index_stream_id,sequence) "
                    "VALUES($1,'reidentify',"
                    "$2,$3,$4)",
                {value.event_id, value.current_stream.repository_id,
                 value.current_stream.index_stream_id, std::to_string(value.sequence)},
                "handoff outbox", PGRES_COMMAND_OK);
        transaction.commit();
        return {ApplyDisposition::Applied,
                {true, value.sequence, value.epoch, manifest, false, false},
                2};
    } catch (const SqlError& error) {
        map_conflict(error);
    }
    throw std::runtime_error("unreachable reidentification state");
}

MaintenanceResult PostgresqlPortfolioStore::maintenance(MaintenanceKind kind) {
    if (kind != MaintenanceKind::Validate)
        fail(PortfolioStoreErrorCode::UnsupportedCapability, "maintenance unsupported");
    std::lock_guard<std::mutex> lock(mutex_);
    auto invalid = execute(connection_,
                           "SELECT count(*) FROM " + table("portfolio_streams") + " WHERE cursor=0",
                           {}, "validate cursors");
    if (number(invalid.get(), 0, 0) != 0)
        fail(PortfolioStoreErrorCode::IdempotencyConflict, "invalid zero cursor");
    auto orphan = execute(
        connection_,
        "SELECT count(*) FROM " + table("portfolio_reidentifications") + " r LEFT JOIN " +
            table("portfolio_events") +
            " e ON e.event_id=r.event_id WHERE e.event_id IS NULL OR e.fingerprint<>r.fingerprint",
        {}, "validate receipts");
    if (number(orphan.get(), 0, 0) != 0)
        fail(PortfolioStoreErrorCode::IdempotencyConflict, "orphan handoff receipt");
    return {kind, true, "postgresql projection is internally consistent"};
}

std::size_t PostgresqlPortfolioStore::pending_outbox_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto count = execute(
        connection_, "SELECT count(*) FROM " + table("portfolio_outbox") + " WHERE published=false",
        {}, "count outbox");
    return static_cast<std::size_t>(number(count.get(), 0, 0));
}

std::unique_ptr<PortfolioStore> make_postgresql_conformance_store() {
    const auto* dsn = std::getenv("AXON_POSTGRES_TEST_DSN");
    if (!dsn || std::string(dsn).empty())
        throw std::runtime_error("AXON_POSTGRES_TEST_DSN is required");
    static std::atomic<std::uint64_t> ordinal{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::make_unique<PostgresqlPortfolioStore>(dsn,
                                                      "axon_test_conformance_" +
                                                          std::to_string(stamp) + "_" +
                                                          std::to_string(ordinal.fetch_add(1)),
                                                      true);
}

} // namespace axon::portfolio
