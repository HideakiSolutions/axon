#include "duckdb_repository_projector.hpp"
#include "repository_reidentification_validation.hpp"

#include <duckdb.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <unordered_set>

namespace axon::portfolio {
namespace {

struct Source {
    std::unique_ptr<duckdb::DuckDB> database;
    std::unique_ptr<duckdb::Connection> connection;
    RepositoryStreamKey stream;
    std::string epoch;
    std::string manifest;
    std::string verified_manifest;
    bool removed = false;
};

struct JournalEvent {
    ProjectionEvent projection;
    std::string event_type;
    std::optional<RepositoryReidentification> reidentification;
};

constexpr const char* kIndexMetadataSchema = "axon/index-metadata/v1";

template <typename Result>
void require_ok(const std::unique_ptr<Result>& result, const char* operation) {
    if (!result || result->HasError())
        throw std::runtime_error(std::string(operation) + ": " +
                                 (result ? result->GetError() : "no DuckDB result"));
}

bool path_has_symlink(const std::filesystem::path& root, const std::filesystem::path& path) {
    auto cursor = root;
    auto relative = path.lexically_relative(root);
    for (const auto& part : relative) {
        cursor /= part;
        std::error_code error;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(cursor, error)) || error)
            return true;
    }
    return false;
}

Source open_source(const std::filesystem::path& registered_root,
                   const std::filesystem::path& index_path) {
    std::error_code error;
    const auto absolute_root = std::filesystem::absolute(registered_root, error).lexically_normal();
    if (error || path_has_symlink(absolute_root.root_path(), absolute_root))
        throw std::invalid_argument("registered repository root contains a symbolic link");
    const auto root = std::filesystem::canonical(registered_root, error);
    if (error || !std::filesystem::is_directory(root))
        throw std::invalid_argument("registered repository root is unavailable");
    const auto absolute_index = std::filesystem::absolute(index_path, error).lexically_normal();
    if (error || path_has_symlink(absolute_root, absolute_index))
        throw std::invalid_argument("index database path contains a symbolic link");
    const auto index = std::filesystem::canonical(index_path, error);
    if (error || !std::filesystem::is_regular_file(index))
        throw std::invalid_argument("registered index database is unavailable");
    const auto relative = index.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..")
        throw std::invalid_argument("index database must be a non-symlink descendant of its root");

    duckdb::DBConfig config;
    config.options.access_mode = duckdb::AccessMode::READ_ONLY;
    Source source;
    source.database = std::make_unique<duckdb::DuckDB>(index.string(), &config);
    source.connection = std::make_unique<duckdb::Connection>(*source.database);
    require_ok(source.connection->Query("BEGIN TRANSACTION"), "begin read-only source snapshot");
    auto metadata = source.connection->Query(
        "SELECT "
        "repository_id,index_stream_id,current_epoch,current_manifest,removed,schema_version "
        "FROM index_metadata WHERE singleton=true");
    require_ok(metadata, "read source metadata");
    if (metadata->RowCount() != 1) throw std::runtime_error("source index identity is missing");
    source.stream = {metadata->GetValue(0, 0).ToString(), metadata->GetValue(1, 0).ToString()};
    source.epoch = metadata->GetValue(2, 0).ToString();
    source.manifest = metadata->GetValue(3, 0).ToString();
    source.removed = metadata->GetValue<bool>(4, 0);
    if (metadata->GetValue(5, 0).ToString() != kIndexMetadataSchema)
        throw std::runtime_error("unsupported source index metadata schema");
    auto verified = source.connection->Prepare(
        "SELECT manifest_hash FROM index_events WHERE index_stream_id=$1 AND "
        "manifest_hash IS NOT NULL ORDER BY sequence DESC LIMIT 1");
    auto verified_result = verified->Execute(source.stream.index_stream_id);
    require_ok(verified_result, "read latest verified source manifest");
    auto verified_row = verified_result->Fetch();
    if (verified_row && verified_row->size() != 0)
        source.verified_manifest = verified_row->GetValue(0, 0).ToString();
    return source;
}

JournalEvent parse_event(const RepositoryStreamKey& physical_stream, duckdb::DataChunk& rows,
                         duckdb::idx_t row) {
    JournalEvent parsed;
    auto& event = parsed.projection;
    event.stream = {rows.GetValue(6, row).ToString(), physical_stream.index_stream_id};
    event.sequence = rows.GetValue(0, row).GetValue<std::uint64_t>();
    event.event_id = rows.GetValue(1, row).ToString();
    event.epoch = rows.GetValue(2, row).ToString();
    if (!rows.GetValue(3, row).IsNull()) event.manifest = rows.GetValue(3, row).ToString();

    parsed.event_type = rows.GetValue(7, row).ToString();
    static const std::unordered_set<std::string> event_types = {
        "IndexSnapshotCompleted", "IndexFilesUpdated",     "IndexFilesDeleted",
        "IndexSymbolsUpdated",    "IndexContractsUpdated", "IndexRoutesUpdated",
        "RepositoryReidentified", "RepositoryRemoved"};
    if (event_types.count(parsed.event_type) == 0)
        throw std::runtime_error("unsupported source index event type");

    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(rows.GetValue(4, row).ToString());
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("invalid payload_json for event " + event.event_id + ": " +
                                 error.what());
    }
    if (!payload.is_object() || !payload.contains("affected") || !payload.at("affected").is_array())
        throw std::runtime_error("invalid index event payload shape");
    if (payload.at("affected").size() > 10000)
        throw std::runtime_error("index event affected set exceeds schema bound");
    for (const auto& [key, value] : payload.items()) {
        (void)value;
        if (key != "affected" && key != "identity_change")
            throw std::runtime_error("unknown index event payload field");
    }
    static const std::unordered_set<std::string> entity_kinds = {
        "file",   "symbol", "contract", "route",      "handler",   "event",
        "schema", "dto",    "test",     "dependency", "tombstone", "repository"};
    bool has_delete = false;
    bool has_repository_delete = false;
    for (const auto& item : payload.at("affected")) {
        if (!item.is_object() || !item.contains("kind") || !item.at("kind").is_string() ||
            !item.contains("key") || !item.at("key").is_string() || !item.contains("operation") ||
            !item.at("operation").is_string() ||
            (item.contains("digest") && !item.at("digest").is_string() &&
             !item.at("digest").is_null()))
            throw std::runtime_error("invalid affected entity shape");
        for (const auto& [key, value] : item.items()) {
            (void)value;
            if (key != "kind" && key != "key" && key != "operation" && key != "digest")
                throw std::runtime_error("unknown affected entity field");
        }
        const auto operation = item.at("operation").get<std::string>();
        const auto kind = item.at("kind").get<std::string>();
        if (entity_kinds.count(kind) == 0) throw std::runtime_error("invalid affected entity kind");
        if (operation != "upsert" && operation != "delete" && operation != "snapshot")
            throw std::runtime_error("invalid affected entity operation");
        has_delete = has_delete || operation == "delete";
        has_repository_delete =
            has_repository_delete || (kind == "repository" && operation == "delete");
        ProjectionMutation mutation{kind, item.at("key").get<std::string>(),
                                    operation == "delete" ? ProjectionOperation::Delete
                                                          : ProjectionOperation::Upsert,
                                    std::nullopt};
        if (item.contains("digest") && !item.at("digest").is_null())
            mutation.digest = item.at("digest").get<std::string>();
        event.mutations.push_back(std::move(mutation));
    }
    if (parsed.event_type == "IndexSnapshotCompleted" && !event.manifest)
        throw std::runtime_error("snapshot event has no verified manifest");
    if (parsed.event_type == "IndexFilesDeleted" && !has_delete)
        throw std::runtime_error("file deletion event has no delete mutation");
    if (parsed.event_type == "RepositoryRemoved" && !has_repository_delete)
        throw std::runtime_error("repository removal event has no repository delete mutation");
    if (parsed.event_type == "RepositoryReidentified") {
        if (!payload.contains("identity_change") || !payload.at("identity_change").is_object())
            throw std::runtime_error("reidentification event has no typed identity change");
        const auto& identity = payload.at("identity_change");
        static const std::unordered_set<std::string> identity_fields = {"old_repository_id",
                                                                        "new_repository_id",
                                                                        "handoff_sequence",
                                                                        "old_binding_id",
                                                                        "new_binding_id",
                                                                        "approval_reference",
                                                                        "reason"};
        for (const auto& field : identity_fields)
            if (!identity.contains(field))
                throw std::runtime_error("reidentification identity field is missing");
        for (const auto& [key, value] : identity.items()) {
            (void)value;
            if (identity_fields.count(key) == 0)
                throw std::runtime_error("unknown reidentification identity field");
        }
        if (!identity.at("old_repository_id").is_string() ||
            !identity.at("new_repository_id").is_string() ||
            !identity.at("handoff_sequence").is_number_unsigned() ||
            !identity.at("old_binding_id").is_string() ||
            !identity.at("new_binding_id").is_string() ||
            !identity.at("approval_reference").is_string() || !identity.at("reason").is_string())
            throw std::runtime_error("invalid reidentification identity shape");
        const auto old_id = identity.at("old_repository_id").get<std::string>();
        const auto new_id = identity.at("new_repository_id").get<std::string>();
        if (identity.at("handoff_sequence").get<std::uint64_t>() != event.sequence ||
            event.stream.repository_id != old_id)
            throw std::runtime_error("reidentification identity does not match journal row");
        bool old_delete = false;
        bool new_upsert = false;
        for (const auto& mutation : event.mutations) {
            if (mutation.digest)
                throw std::runtime_error(
                    "reidentification identity mutations cannot carry digests");
            old_delete = old_delete ||
                         (mutation.entity_kind == "repository" && mutation.entity_key == old_id &&
                          mutation.operation == ProjectionOperation::Delete);
            new_upsert = new_upsert ||
                         (mutation.entity_kind == "repository" && mutation.entity_key == new_id &&
                          mutation.operation == ProjectionOperation::Upsert);
        }
        if (event.mutations.size() != 2 || !old_delete || !new_upsert)
            throw std::runtime_error("reidentification affected set is not canonical");
        parsed.reidentification =
            RepositoryReidentification{{old_id, physical_stream.index_stream_id},
                                       {new_id, physical_stream.index_stream_id},
                                       event.sequence,
                                       event.event_id,
                                       event.epoch,
                                       event.manifest,
                                       identity.at("old_binding_id").get<std::string>(),
                                       identity.at("new_binding_id").get<std::string>(),
                                       identity.at("approval_reference").get<std::string>(),
                                       identity.at("reason").get<std::string>()};
    } else if (payload.contains("identity_change")) {
        throw std::runtime_error("identity_change is only valid for reidentification events");
    }
    return parsed;
}

std::vector<JournalEvent> read_events(Source& source, std::uint64_t after, std::size_t limit) {
    auto statement = source.connection->Prepare(
        "SELECT sequence,event_id,index_epoch,manifest_hash,payload_json,schema_version,"
        "repository_id,event_type FROM index_events WHERE index_stream_id=$1 AND sequence>$2 "
        "ORDER BY sequence LIMIT $3");
    auto result =
        statement->Execute(source.stream.index_stream_id, after, static_cast<std::int64_t>(limit));
    require_ok(result, "read source journal");
    std::vector<JournalEvent> events;
    while (auto rows = result->Fetch())
        for (duckdb::idx_t row = 0; row < rows->size(); ++row)
            if (rows->GetValue(5, row).ToString() != "axon/index-event/v1")
                throw std::runtime_error("unsupported source index event schema");
            else
                events.push_back(parse_event(source.stream, *rows, row));
    return events;
}

void validate_source_identity_chain(Source& source) {
    std::optional<std::string> logical_repository_id;
    std::uint64_t cursor = 0;
    for (;;) {
        const auto events = read_events(source, cursor, 500);
        if (events.empty()) break;
        for (const auto& event : events) {
            if (event.projection.sequence != cursor + 1)
                throw std::runtime_error("source journal sequence is not contiguous");
            if (!logical_repository_id)
                logical_repository_id = event.projection.stream.repository_id;
            if (event.projection.stream.repository_id != *logical_repository_id)
                throw std::runtime_error("source journal logical identity chain is invalid");
            if (event.reidentification) {
                const auto validation =
                    duckdb_detail::validate_reidentification(*event.reidentification, cursor);
                if (!validation) throw std::runtime_error(validation.message);
                logical_repository_id = event.reidentification->current_stream.repository_id;
            }
            cursor = event.projection.sequence;
        }
    }
    if (logical_repository_id && *logical_repository_id != source.stream.repository_id)
        throw std::runtime_error("source metadata identity diverges from its journal");
}

struct AdvanceResult {
    std::uint64_t cursor = 0;
    std::size_t applied = 0;
};

AdvanceResult advance(Source& source, PortfolioStore& store, std::uint64_t cursor,
                      std::size_t batch_size) {
    AdvanceResult result{cursor, 0};
    for (;;) {
        auto events = read_events(source, result.cursor, batch_size);
        if (events.empty()) break;
        const auto handoff = std::find_if(events.begin(), events.end(), [](const auto& event) {
            return event.reidentification.has_value();
        });
        if (handoff == events.begin()) {
            const auto applied =
                store.reidentify_repository_stream(*handoff->reidentification, result.cursor);
            result.cursor = applied.state.cursor;
            if (applied.disposition == ApplyDisposition::Applied) ++result.applied;
            continue;
        }

        // A reidentification is an explicit transaction boundary because it atomically moves the
        // physical stream to another logical partition. The ordinary prefix (or the entire fetch
        // when there is no handoff) is committed with its cursor in one store transaction.
        std::vector<ProjectionEvent> batch;
        batch.reserve(static_cast<std::size_t>(std::distance(events.begin(), handoff)));
        for (auto event = events.begin(); event != handoff; ++event)
            batch.push_back(event->projection);
        const auto applied = store.apply(batch.front().stream, result.cursor, batch);
        result.cursor = applied.state.cursor;
        if (applied.disposition == ApplyDisposition::Applied) result.applied += batch.size();
    }
    return result;
}

RepositorySnapshot fold_source(Source& source) {
    std::map<std::pair<std::string, std::string>, ProjectionMutation> entities;
    std::uint64_t cursor = 0;
    std::string epoch;
    std::string manifest;
    for (;;) {
        auto events = read_events(source, cursor, 500);
        if (events.empty()) break;
        for (const auto& journal_event : events) {
            const auto& event = journal_event.projection;
            cursor = event.sequence;
            epoch = event.epoch;
            if (event.manifest) manifest = *event.manifest;
            for (const auto& mutation : event.mutations) {
                auto key = std::make_pair(mutation.entity_kind, mutation.entity_key);
                if (mutation.operation == ProjectionOperation::Delete)
                    entities.erase(key);
                else
                    entities[key] = mutation;
            }
        }
    }
    if (cursor == 0 || manifest.empty() || (!source.epoch.empty() && epoch != source.epoch))
        throw std::runtime_error("source journal has no rebuildable verified state");
    std::vector<ProjectionMutation> flattened;
    for (const auto& [key, entity] : entities)
        flattened.push_back(entity);
    return {source.stream, cursor, epoch, manifest, false, source.removed, std::move(flattened)};
}

} // namespace

RepositoryProjectionResult
DuckdbRepositoryProjector::sync(const std::filesystem::path& registered_root,
                                const std::filesystem::path& index_path, std::size_t batch_size) {
    if (batch_size == 0 || batch_size > 500) throw std::invalid_argument("invalid sync batch size");
    auto source = open_source(registered_root, index_path);
    validate_source_identity_chain(source);
    auto state = store_.stream_state(source.stream);
    RepositoryProjectionResult result{source.stream, state.cursor, state.cursor, 0, false, false};
    const auto advanced = advance(source, store_, result.cursor_after, batch_size);
    result.cursor_after = advanced.cursor;
    result.events_applied = advanced.applied;
    const auto final_state = store_.stream_state(source.stream);
    if (final_state.cursor != 0 &&
        ((!source.epoch.empty() && final_state.epoch != source.epoch) ||
         (!source.verified_manifest.empty() && final_state.manifest != source.verified_manifest) ||
         final_state.removed != source.removed))
        return rebuild(registered_root, index_path);
    if (final_state.exists && final_state.stale) {
        const auto limit = store_.capabilities().max_snapshot_entities;
        if (limit == 0 || limit > 10000)
            throw std::runtime_error("provider advertises an invalid stale recovery bound");
        const auto projection = store_.inspect_repository_stream(source.stream, limit);
        if (projection.truncated)
            throw std::runtime_error("stale partition exceeds bounded recovery replace");
        const RepositorySnapshot recovered = {
            source.stream, final_state.cursor,  final_state.epoch,  final_state.manifest,
            false,         final_state.removed, projection.entities};
        store_.replace_repository_stream(recovered, final_state.cursor);
    }
    return result;
}

RepositoryProjectionResult
DuckdbRepositoryProjector::rebuild(const std::filesystem::path& registered_root,
                                   const std::filesystem::path& index_path) {
    auto source = open_source(registered_root, index_path);
    validate_source_identity_chain(source);
    auto before = store_.stream_state(source.stream);
    if (!before.exists) {
        (void)advance(source, store_, 0, 500);
        before = store_.stream_state(source.stream);
    }
    auto snapshot = fold_source(source);
    store_.replace_repository_stream(snapshot, before.cursor);
    return {source.stream, before.cursor, snapshot.cursor, 0, true, false};
}

bool DuckdbRepositoryProjector::mark_stale(const RepositoryStreamKey& stream) {
    const auto state = store_.stream_state(stream);
    if (!state.exists || state.cursor == 0 || state.stale) return false;
    const auto limit = store_.capabilities().max_snapshot_entities;
    if (limit == 0 || limit > 10000)
        throw std::runtime_error("provider advertises an invalid stale snapshot bound");
    const auto projection = store_.inspect_repository_stream(stream, limit);
    if (projection.truncated)
        throw std::runtime_error("stale partition exceeds provider snapshot bound");
    RepositorySnapshot snapshot{stream, state.cursor,  state.epoch,        state.manifest,
                                true,   state.removed, projection.entities};
    store_.replace_repository_stream(snapshot, state.cursor);
    return true;
}

} // namespace axon::portfolio
