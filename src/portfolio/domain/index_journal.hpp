#pragma once

#include <duckdb.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace axon::portfolio {

struct IndexIdentity {
    std::string repository_id;
    std::string index_stream_id;
    std::string current_epoch;
    std::string current_manifest;
    bool removed = false;
};

struct IdentityChange {
    std::string old_repository_id;
    std::string new_repository_id;
    std::string old_binding_id;
    std::string new_binding_id;
    std::string approval_reference;
    std::string reason;
};

struct AffectedEntity {
    std::string kind;
    std::string key;
    std::string operation;
    std::optional<std::string> digest;
};

class Transaction {
public:
    explicit Transaction(duckdb::Connection& connection);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void mark_index_mutation();
    void mark_event_appended();
    void commit();

private:
    duckdb::Connection& connection_;
    bool committed_ = false;
    bool mutation_observed_ = false;
    bool event_observed_ = false;
};

IndexIdentity index_identity(duckdb::Connection& connection);
std::string compute_manifest_hash(duckdb::Connection& connection);
std::string compute_index_epoch(const IndexIdentity& identity, const std::string& manifest_hash);
uint64_t append_index_event(Transaction& transaction, duckdb::Connection& connection,
                            const std::string& event_type,
                            const std::vector<AffectedEntity>& affected,
                            const std::string& manifest_hash,
                            const std::optional<std::string>& source_ref = std::nullopt);
void upsert_tombstone(duckdb::Connection& connection, const AffectedEntity& entity,
                      uint64_t deleted_sequence, const std::string& deleted_epoch);
void clear_tombstone(duckdb::Connection& connection, const AffectedEntity& entity);
uint64_t reidentify_repository(Transaction& transaction, duckdb::Connection& connection,
                               const IdentityChange& change);
uint64_t remove_repository(Transaction& transaction, duckdb::Connection& connection,
                           const std::optional<std::string>& source_ref = std::nullopt);

void set_journal_failpoint_for_testing(const std::string& failpoint);
void clear_journal_failpoint_for_testing();
void trigger_journal_failpoint_for_testing(const std::string& failpoint);

} // namespace axon::portfolio
