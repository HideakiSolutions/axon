#pragma once
#include <duckdb.hpp>
#include <filesystem>
#include <string>
#include <stdexcept>

namespace axon {

bool is_database_lock_error(const std::string& message);
std::string database_open_error_message(const std::filesystem::path& db_path,
                                        const std::exception& error);

// Run query and return MaterializedQueryResult or throw
inline duckdb::MaterializedQueryResult& require_ok(
    std::unique_ptr<duckdb::MaterializedQueryResult>& res,
    const std::string& ctx = "")
{
    if (res->HasError())
        throw std::runtime_error("DuckDB error" + (ctx.empty() ? "" : " [" + ctx + "]") +
                                 ": " + res->GetError());
    return *res;
}

// Helper: run a query and return materialized result
inline std::unique_ptr<duckdb::MaterializedQueryResult> dq(duckdb::Connection& c, const std::string& sql) {
    return c.Query(sql);
}

// Execute prepared statement without streaming
inline std::unique_ptr<duckdb::QueryResult> exec_stmt(
    duckdb::PreparedStatement& stmt,
    duckdb::vector<duckdb::Value> vals)
{
    return stmt.Execute(vals, false);
}

class Database {
public:
    explicit Database(const std::filesystem::path& db_path);

    duckdb::Connection& conn() { return *conn_; }

    // Execute SQL with no return value (DDL, DML)
    void exec(const std::string& sql);

    // Execute SQL and return materialized result
    duckdb::MaterializedQueryResult& query(const std::string& sql);

    void run_migrations();

private:
    duckdb::DuckDB db_;
    std::unique_ptr<duckdb::Connection> conn_;
    std::unique_ptr<duckdb::MaterializedQueryResult> last_result_;  // keeps result alive
};

} // namespace axon
