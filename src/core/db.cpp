#include "db.hpp"
#include <iostream>

namespace axon {

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
}

} // namespace axon
