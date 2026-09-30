#include <duckdb.hpp>
#include <iostream>
#include <string>

// Build a larger read-only retrieval workload from one indexed fixture.
// The runner copies the DuckDB file before calling this helper; never point
// it at a project's authoritative index.
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const int copies = std::stoi(argv[2]);
    if (copies < 1 || copies > 20) return 2;
    duckdb::DuckDB db(argv[1]);
    duckdb::Connection conn(db);
    auto max_id = conn.Query("SELECT GREATEST((SELECT MAX(id) FROM files),"
                             "(SELECT MAX(id) FROM symbols))");
    if (max_id->HasError()) return 1;
    const int64_t step = max_id->GetValue<int64_t>(0, 0) + 1;
    for (int i = 1; i < copies; ++i) {
        const auto offset = std::to_string(step * i);
        const auto prefix = "copy_" + std::to_string(i) + "/";
        auto files = conn.Query(
            "INSERT INTO files (id,path,language,hash,indexed_at,byte_size,skeleton) "
            "SELECT id+" + offset + ",'" + prefix + "'||path,language,hash,indexed_at,"
            "byte_size,skeleton FROM files WHERE id<" + std::to_string(step));
        if (files->HasError()) { std::cerr << files->GetError() << '\n'; return 1; }
        auto symbols = conn.Query(
            "INSERT INTO symbols (id,file_id,name,kind,start_line,end_line,signature,docstring,"
            "search_terms,search_length,embedding) SELECT id+" + offset + ",file_id+" +
            offset + ",name,kind,start_line,end_line,signature,docstring,search_terms,"
            "search_length,embedding FROM symbols WHERE id<" + std::to_string(step));
        if (symbols->HasError()) { std::cerr << symbols->GetError() << '\n'; return 1; }
        auto postings = conn.Query(
            "INSERT INTO symbol_terms SELECT symbol_id+" + offset +
            ",term,tf FROM symbol_terms WHERE symbol_id<" + std::to_string(step));
        if (postings->HasError()) { std::cerr << postings->GetError() << '\n'; return 1; }
    }
}
