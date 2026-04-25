#include "server.hpp"
#include "protocol.hpp"
#include "../core/indexer.hpp"
#include "../core/capsule.hpp"
#include "../core/skeleton.hpp"
#include "../core/embeddings.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <filesystem>
#include <unordered_set>
#include <algorithm>

namespace {
inline std::string sql_escape(const std::string& s) {
    std::string out; out.reserve(s.size() + 4);
    for (char c : s) { if (c == '\'') out += '\''; out += c; }
    return out;
}
}

namespace axon::mcp {

static json tools_list() {
    return {{"tools", json::array({
        {{"name","run_pipeline"},
         {"description","Index the project: parse source files, build dependency graph, compute embeddings."},
         {"inputSchema",{{"type","object"},{"properties",{{"root",{{"type","string"}}}}}}}},
        {{"name","index_paths"},
         {"description","Incrementally reindex specific files for write-through after Write/Edit. Pass an empty paths array with prune=true to only sweep deleted files from the index."},
         {"inputSchema",{{"type","object"},{"required",{"paths"}},{"properties",{
             {"paths",{{"type","array"},{"items",{{"type","string"}}}}},
             {"prune",{{"type","boolean"},{"default",false}}}}}}}},
        {{"name","get_context_capsule"},
         {"description","Return token-efficient context: pivot files in full + support files skeletonized."},
         {"inputSchema",{{"type","object"},{"properties",{
             {"query",{{"type","string"}}},
             {"pivot_files",{{"type","array"},{"items",{{"type","string"}}}}},
             {"token_budget",{{"type","integer"},{"default",8000}}}}}}}},
        {{"name","get_impact_graph"},
         {"description","Return files that depend on (or are depended on by) the given files."},
         {"inputSchema",{{"type","object"},{"required",{"files"}},{"properties",{
             {"files",{{"type","array"},{"items",{{"type","string"}}}}}}}}}},
        {{"name","get_skeleton"},
         {"description","Return signatures-only view of one or more files."},
         {"inputSchema",{{"type","object"},{"required",{"files"}},{"properties",{
             {"files",{{"type","array"},{"items",{{"type","string"}}}}}}}}}},
        {{"name","search_memory"},
         {"description","Semantic search over saved observations."},
         {"inputSchema",{{"type","object"},{"required",{"query"}},{"properties",{
             {"query",{{"type","string"}}},
             {"limit",{{"type","integer"},{"default",5}}}}}}}},
        {{"name","save_observation"},
         {"description","Persist a text observation for future retrieval."},
         {"inputSchema",{{"type","object"},{"required",{"content"}},{"properties",{
             {"content",{{"type","string"}}},
             {"tags",{{"type","array"},{"items",{{"type","string"}}}}},
             {"file_path",{{"type","string"}}}}}}}},
        {{"name","get_overview"},
         {"description","Return codebase entry points: top files by coupling (incoming+outgoing edges) and top referenced symbols. Use for onboarding / vibe coding when no specific query is formed yet."},
         {"inputSchema",{{"type","object"},{"properties",{
             {"limit",{{"type","integer"},{"default",10}}}}}}}},
        {{"name","get_callers"},
         {"description","Backward trace (file-granular): locates the symbol by name, then returns the list of files that import the file defining it. Narrow to concrete call sites with get_skeleton(caller_files) afterwards. Use for debugging and root-cause analysis."},
         {"inputSchema",{{"type","object"},{"required",{"symbol_name"}},{"properties",{
             {"symbol_name",{{"type","string"}}},
             {"file_path",{{"type","string"},{"description","Optional: disambiguate when multiple symbols share a name"}}},
             {"limit",{{"type","integer"},{"default",50}}}}}}}},
        {{"name","get_tests_for"},
         {"description","Return test files (by path convention) that import/reference the given files. Use for test-impact analysis before merging."},
         {"inputSchema",{{"type","object"},{"required",{"files"}},{"properties",{
             {"files",{{"type","array"},{"items",{{"type","string"}}}}}}}}}},
    })}};
}

// Drain the PostToolUse pending-writes queue ($PROJECT/.axon/pending-writes.txt)
// Called at the start of every tool call so the Claude Code hook's write-through
// is visible by the time any MCP tool actually looks at the index. The hook owns
// the queue file; we atomically steal it (rename to tmp) before processing to
// avoid racing the hook's flock on new appends.
static int drain_pending_writes(ServerContext& ctx) {
    namespace fs = std::filesystem;
    if (!ctx.db_ready()) return 0;

    fs::path queue = ctx.cfg.axon_dir / "pending-writes.txt";
    std::error_code ec;
    if (!fs::exists(queue, ec) || fs::file_size(queue, ec) == 0) return 0;

    // Atomically claim the queue — any concurrent hook append after this rename
    // writes to a fresh empty file we'll pick up next drain.
    fs::path claimed = ctx.cfg.axon_dir / "pending-writes.processing";
    fs::remove(claimed, ec);
    fs::rename(queue, claimed, ec);
    if (ec) return 0;

    std::vector<fs::path> paths;
    {
        std::ifstream in(claimed);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            paths.emplace_back(line);
        }
    }
    fs::remove(claimed, ec);

    if (paths.empty()) return 0;

    // Deduplicate while preserving order — same file edited N times in a row
    // should only be reindexed once.
    std::vector<fs::path> unique;
    std::unordered_set<std::string> seen;
    for (auto& p : paths) {
        auto key = p.string();
        if (seen.insert(key).second) unique.push_back(std::move(p));
    }

    auto stats = index_files(ctx.cfg, *ctx.db, unique, false);
    if (stats.files_indexed > 0 && ctx.model_ready()) {
        try { embed_pending_symbols(*ctx.db, *ctx.model); }
        catch (...) { /* silent — will retry on next drain */ }
    }
    if (stats.files_indexed > 0)
        ctx.graph = load_graph(*ctx.db);

    return stats.files_indexed;
}

// Filesystem-change signal: PostToolUse(Bash) and UserPromptSubmit hooks touch
// .axon/sync-requested whenever an opaque filesystem change may have happened
// (rm, mv, git checkout, generated files, etc). On the next tool call we do a
// full walk + BLAKE3-skip upsert + sweep, converging the index with disk in
// one pass. Cheap in steady state because unchanged files cost only a hash.
static void maybe_run_sync(ServerContext& ctx) {
    namespace fs = std::filesystem;
    if (!ctx.db_ready()) return;
    fs::path marker = ctx.cfg.axon_dir / "sync-requested";
    std::error_code ec;
    if (!fs::exists(marker, ec)) return;
    fs::remove(marker, ec);

    auto stats = sync_project(ctx.cfg, *ctx.db);
    if (stats.files_indexed > 0 && ctx.model_ready()) {
        try { embed_pending_symbols(*ctx.db, *ctx.model); }
        catch (...) { /* silent — will retry next drain */ }
    }
    if (stats.files_indexed > 0 || stats.files_pruned > 0)
        ctx.graph = load_graph(*ctx.db);
}

static json handle_tool(const std::string& name, const json& args, ServerContext& ctx) {
    // Order matters: sync first (full walk detects mv/rm/new-file side-effects),
    // then drain pending-writes (re-resolves edges for files that already have
    // peers in the DB after sync). Reversed order loses edges when a Write and
    // a Bash mv/rename happen in the same turn — drain would process the edited
    // file before sync has inserted the new peer, so resolve_edges misses.
    maybe_run_sync(ctx);
    drain_pending_writes(ctx);

    if (name == "run_pipeline") {
        if (!ctx.db_ready())
            return make_tool_result({{"error","DB not initialized"}}, true);

        auto stats = index_project(ctx.cfg, *ctx.db);
        ctx.graph = load_graph(*ctx.db);

        if (!ctx.model_ready()) {
            try {
                auto mp = find_model(ctx.cfg.project_root / "models");
                ctx.model = std::make_unique<EmbeddingModel>(mp);
            } catch (const std::exception& e) {
                return make_tool_result({
                    {"warning", std::string("Indexed without embeddings: ") + e.what()},
                    {"files_indexed", stats.files_indexed},
                    {"symbols_found", stats.symbols_found}
                });
            }
        }
        return make_tool_result({
            {"files_indexed", stats.files_indexed},
            {"symbols_found", stats.symbols_found},
            {"edges_found",   stats.edges_found}
        });
    }

    if (name == "index_paths") {
        if (!ctx.db_ready())
            return make_tool_result({{"error","DB not initialized"}}, true);

        std::vector<std::filesystem::path> paths;
        if (args.contains("paths"))
            for (const auto& p : args["paths"]) paths.emplace_back(p.get<std::string>());
        bool prune = args.value("prune", false);

        auto stats = index_files(ctx.cfg, *ctx.db, paths, prune);

        // If we inserted new symbols and a model is available, embed them inline
        // so the next semantic query already sees them.
        int embedded = 0;
        if (stats.files_indexed > 0 && ctx.model_ready()) {
            try {
                embedded = embed_pending_symbols(*ctx.db, *ctx.model);
            } catch (const std::exception& e) {
                return make_tool_result({
                    {"warning", std::string("Indexed but embedding failed: ") + e.what()},
                    {"files_indexed", stats.files_indexed},
                    {"files_skipped", stats.files_skipped},
                    {"files_pruned",  stats.files_pruned}
                });
            }
        }

        // Refresh in-memory graph so subsequent get_impact_graph sees new edges
        if (stats.files_indexed > 0 || stats.files_pruned > 0)
            ctx.graph = load_graph(*ctx.db);

        return make_tool_result({
            {"files_indexed",    stats.files_indexed},
            {"files_skipped",    stats.files_skipped},
            {"files_pruned",     stats.files_pruned},
            {"symbols_found",    stats.symbols_found},
            {"symbols_embedded", embedded}
        });
    }

    if (name == "get_context_capsule") {
        if (!ctx.db_ready() || !ctx.model_ready())
            return make_tool_result({{"error","Run run_pipeline first"}}, true);

        std::string query = args.value("query", "");
        int budget = args.value("token_budget", 8000);
        std::vector<std::string> pivots;
        if (args.contains("pivot_files"))
            for (const auto& p : args["pivot_files"]) pivots.push_back(p.get<std::string>());

        auto capsule = assemble_capsule(query, pivots, *ctx.db, *ctx.model,
                                        ctx.graph, ctx.cfg.project_root, budget);

        json pf = json::array();
        for (const auto& f : capsule.pivot_files)
            pf.push_back({{"path",f.path},{"content",f.content},{"tokens",f.token_estimate}});
        json sf = json::array();
        for (const auto& f : capsule.support_files)
            sf.push_back({{"path",f.path},{"content",f.content},{"tokens",f.token_estimate}});

        return make_tool_result({
            {"query", capsule.query},
            {"pivot_files", pf},
            {"support_files", sf},
            {"token_estimate", capsule.token_estimate},
            {"total_files_indexed", capsule.total_files}
        });
    }

    if (name == "get_impact_graph") {
        if (!ctx.db_ready())
            return make_tool_result({{"error","Run run_pipeline first"}}, true);

        json result = json::array();
        for (const auto& p : args["files"]) {
            std::string path = p.get<std::string>();
            auto it = ctx.graph.path_to_id.find(path);
            if (it == ctx.graph.path_to_id.end()) continue;

            json dependents = json::array();
            auto in = ctx.graph.incoming.find(it->second);
            if (in != ctx.graph.incoming.end())
                for (int64_t dep : in->second) {
                    auto p2 = ctx.graph.id_to_path.find(dep);
                    if (p2 != ctx.graph.id_to_path.end()) dependents.push_back(p2->second);
                }
            result.push_back({{"file", path}, {"depended_on_by", dependents}});
        }
        return make_tool_result(result);
    }

    if (name == "get_skeleton") {
        if (!ctx.db_ready())
            return make_tool_result({{"error","Run run_pipeline first"}}, true);

        json result = json::array();

        // Build escaped IN clause
        std::ostringstream in_clause;
        in_clause << "(";
        bool first = true;
        for (const auto& p : args["files"]) {
            std::string path = p.get<std::string>();
            std::string escaped;
            for (char c : path) { if (c == '\'') escaped += '\''; escaped += c; }
            if (!first) in_clause << ",";
            in_clause << "'" << escaped << "'";
            first = false;
        }
        in_clause << ")";

        // Fetch cached skeletons from DB in one query
        auto sk_res = ctx.db->conn().Query(
            "SELECT path, skeleton FROM files WHERE path IN " + in_clause.str());
        auto& sk_mat = *sk_res;
        std::unordered_map<std::string, std::string> cached;
        for (duckdb::idx_t i = 0; i < sk_mat.RowCount(); i++) {
            cached[sk_mat.GetValue(0, i).ToString()] = sk_mat.GetValue(1, i).ToString();
        }

        for (const auto& p : args["files"]) {
            std::string path = p.get<std::string>();
            auto it = cached.find(path);
            if (it != cached.end() && !it->second.empty()) {
                result.push_back({{"path", path}, {"skeleton", it->second}});
            } else {
                // Fallback: re-parse from disk if not in cache
                auto abs = ctx.cfg.project_root / path;
                std::ifstream f(abs, std::ios::binary);
                if (!f) continue;
                std::string content((std::istreambuf_iterator<char>(f)), {});

                // Resolve language
                auto lang_res = ctx.db->conn().Query(
                    "SELECT language FROM files WHERE path = '" + [&]{
                        std::string e; for (char c : path) { if(c=='\'') e+='\''; e+=c; } return e;
                    }() + "'");
                auto& mat = *lang_res;
                if (mat.RowCount() == 0) continue;
                std::string ls = mat.GetValue(0, 0).ToString();
                Language lang = Language::TypeScript;
                if (ls == "python")          lang = Language::Python;
                else if (ls == "rust")       lang = Language::Rust;
                else if (ls == "go")         lang = Language::Go;
                else if (ls == "javascript") lang = Language::JavaScript;
                else if (ls == "csharp")     lang = Language::CSharp;
                else if (ls == "php")        lang = Language::PHP;
                else if (ls == "dart")       lang = Language::Dart;
                else if (ls == "java")       lang = Language::Java;

                result.push_back({{"path", path}, {"skeleton", skeletonize(content, lang)}});
            }
        }
        return make_tool_result(result);
    }

    if (name == "search_memory") {
        if (!ctx.db_ready() || !ctx.model_ready())
            return make_tool_result({{"error","Run run_pipeline first"}}, true);

        std::string q = args.value("query", "");
        int limit = args.value("limit", 5);
        auto qvec = ctx.model->embed(q);

        std::ostringstream vs;
        vs << "[";
        for (size_t i = 0; i < qvec.size(); i++) { if (i) vs << ","; vs << qvec[i]; }
        vs << "]";

        std::string sql = "SELECT content, file_path, created_at FROM observations "
            "WHERE embedding IS NOT NULL "
            "ORDER BY array_cosine_similarity(embedding, " + vs.str() +
            "::FLOAT[" + std::to_string(ctx.model->dims()) + "]) DESC LIMIT " +
            std::to_string(limit);

        auto res = ctx.db->conn().Query(sql);
        auto& mat = *res;

        json result = json::array();
        for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
            result.push_back({
                {"content",    mat.GetValue(0, i).ToString()},
                {"file_path",  mat.GetValue(1, i).ToString()},
                {"created_at", mat.GetValue(2, i).ToString()}
            });
        }
        return make_tool_result(result);
    }

    if (name == "save_observation") {
        if (!ctx.db_ready())
            return make_tool_result({{"error","Run run_pipeline first"}}, true);

        std::string content   = args.value("content", "");
        std::string file_path = args.value("file_path", "");

        // Escape helper for inline SQL strings
        auto sq = [](const std::string& s) {
            std::string out; out.reserve(s.size() + 4);
            for (char c : s) { if (c == '\'') out += '\''; out += c; }
            return out;
        };

        if (ctx.model_ready()) {
            auto emb = ctx.model->embed(content);
            std::ostringstream vs;
            vs << "[";
            for (size_t i = 0; i < emb.size(); i++) { if (i) vs << ","; vs << emb[i]; }
            vs << "]";

            std::string sql =
                "INSERT INTO observations (id, content, file_path, embedding, created_at) VALUES ("
                "nextval('seq_id'), '" + sq(content) + "', '" + sq(file_path) + "', " +
                vs.str() + "::FLOAT[" + std::to_string(ctx.model->dims()) + "], now())";
            ctx.db->conn().Query(sql);
        } else {
            auto stmt = ctx.db->conn().Prepare(
                "INSERT INTO observations (id, content, file_path, created_at) "
                "VALUES (nextval('seq_id'), $1, $2, now())");
            stmt->Execute(content, file_path);
        }
        return make_tool_result({{"saved", true}});
    }

    if (name == "get_overview") {
        if (!ctx.db_ready())
            return make_tool_result({{"error","Run run_pipeline first"}}, true);

        int limit = args.value("limit", 10);
        if (limit < 1)   limit = 1;
        if (limit > 100) limit = 100;

        auto top = ctx.graph.top_files_by_degree(limit);

        // Total indexed file count
        auto total_res = ctx.db->conn().Query("SELECT COUNT(*) FROM files");
        int64_t total_files = total_res->RowCount() > 0
            ? total_res->GetValue(0, 0).GetValue<int64_t>() : 0;

        json top_files = json::array();
        for (const auto& n : top) {
            int in_deg = 0, out_deg = 0;
            auto it_in  = ctx.graph.incoming.find(n.file_id);
            auto it_out = ctx.graph.outgoing.find(n.file_id);
            if (it_in  != ctx.graph.incoming.end()) in_deg  = (int)it_in->second.size();
            if (it_out != ctx.graph.outgoing.end()) out_deg = (int)it_out->second.size();
            int64_t bsize = 0;
            auto it_sz = ctx.graph.file_byte_size.find(n.file_id);
            if (it_sz != ctx.graph.file_byte_size.end()) bsize = it_sz->second;

            top_files.push_back({
                {"path",         n.path},
                {"in_degree",    in_deg},
                {"out_degree",   out_deg},
                {"total_degree", n.degree},
                {"byte_size",    bsize}
            });
        }

        // Top referenced symbols (by number of edges into their file, grouped by name+file)
        json top_symbols = json::array();
        std::string sym_sql =
            "SELECT s.name, s.kind, f.path, s.start_line, s.signature, "
            "       (SELECT COUNT(*) FROM edges e WHERE e.to_file = f.id) AS refs "
            "FROM symbols s JOIN files f ON s.file_id = f.id "
            "WHERE s.kind IN ('function','class','method','interface','type','struct') "
            "ORDER BY refs DESC, s.name "
            "LIMIT " + std::to_string(limit);
        auto sym_res = ctx.db->conn().Query(sym_sql);
        if (!sym_res->HasError()) {
            auto& mat = *sym_res;
            for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
                top_symbols.push_back({
                    {"name",       mat.GetValue(0, i).ToString()},
                    {"kind",       mat.GetValue(1, i).ToString()},
                    {"file",       mat.GetValue(2, i).ToString()},
                    {"line",       mat.GetValue(3, i).GetValue<int32_t>()},
                    {"signature",  mat.GetValue(4, i).ToString()},
                    {"file_refs",  mat.GetValue(5, i).GetValue<int64_t>()}
                });
            }
        }

        return make_tool_result({
            {"total_files_indexed", total_files},
            {"top_files",           top_files},
            {"top_symbols",         top_symbols},
            {"hint", "Use get_context_capsule(query) with a query derived from these entry points, or get_skeleton(files) to inspect signatures."}
        });
    }

    if (name == "get_callers") {
        if (!ctx.db_ready())
            return make_tool_result({{"error","Run run_pipeline first"}}, true);

        std::string sym_name = args.value("symbol_name", "");
        std::string file_hint = args.value("file_path", "");
        int limit = args.value("limit", 50);
        if (sym_name.empty())
            return make_tool_result({{"error","symbol_name is required"}}, true);
        if (limit < 1)    limit = 1;
        if (limit > 500)  limit = 500;

        // Find all symbols matching the name (optionally filtered to a specific file)
        std::string sym_sql =
            "SELECT s.id, s.file_id, s.name, s.kind, s.start_line, s.signature, f.path "
            "FROM symbols s JOIN files f ON s.file_id = f.id "
            "WHERE s.name = '" + sql_escape(sym_name) + "'";
        if (!file_hint.empty())
            sym_sql += " AND f.path = '" + sql_escape(file_hint) + "'";
        sym_sql += " LIMIT 20";

        auto sym_res = ctx.db->conn().Query(sym_sql);
        if (sym_res->HasError())
            return make_tool_result({{"error", sym_res->GetError()}}, true);

        json matches = json::array();
        auto& sm = *sym_res;
        int total_callers = 0;

        for (duckdb::idx_t i = 0; i < sm.RowCount(); i++) {
            int64_t file_id = sm.GetValue(1, i).GetValue<int64_t>();
            std::string callee_file = sm.GetValue(6, i).ToString();

            // File-level callers: files that import the file containing the symbol.
            // Edges are file-granular in the current schema, so this is the tightest
            // lower bound on true caller set. The agent should narrow with get_skeleton.
            json callers = json::array();
            auto it = ctx.graph.incoming.find(file_id);
            if (it != ctx.graph.incoming.end()) {
                for (int64_t src_id : it->second) {
                    auto pit = ctx.graph.id_to_path.find(src_id);
                    if (pit == ctx.graph.id_to_path.end()) continue;
                    callers.push_back(pit->second);
                    total_callers++;
                    if (total_callers >= limit) break;
                }
            }

            // Symbol-level callers (populated when granularity=symbol)
            json caller_symbols = json::array();
            int64_t sym_id = sm.GetValue<int64_t>(0, i);
            auto sym_it = ctx.graph.symbol_incoming.find(sym_id);
            if (sym_it != ctx.graph.symbol_incoming.end()) {
                for (int64_t src_sym_id : sym_it->second) {
                    // Buscar detalhes do símbolo chamador
                    auto src_res = ctx.db->conn().Query(
                        "SELECT s.name, s.kind, f.path, s.start_line "
                        "FROM symbols s JOIN files f ON s.file_id = f.id "
                        "WHERE s.id = " + std::to_string(src_sym_id));
                    if (!src_res->HasError() && src_res->RowCount() > 0) {
                        caller_symbols.push_back({
                            {"name", src_res->GetValue(0, 0).ToString()},
                            {"kind", src_res->GetValue(1, 0).ToString()},
                            {"file", src_res->GetValue(2, 0).ToString()},
                            {"line", src_res->GetValue(3, 0).GetValue<int32_t>()}
                        });
                    }
                }
            }

            matches.push_back({
                {"symbol",         sm.GetValue(2, i).ToString()},
                {"kind",           sm.GetValue(3, i).ToString()},
                {"file",           callee_file},
                {"line",           sm.GetValue(4, i).GetValue<int32_t>()},
                {"signature",      sm.GetValue(5, i).ToString()},
                {"caller_files",   callers},
                {"caller_symbols", caller_symbols}
            });
            if (total_callers >= limit) break;
        }

        return make_tool_result({
            {"symbol_name",    sym_name},
            {"matches",        matches},
            {"note", "caller_files: files importing the defining file. caller_symbols: symbol-level callers (populated when granularity=symbol). Use get_skeleton(caller_files) to narrow to specific call sites."}
        });
    }

    if (name == "get_tests_for") {
        if (!ctx.db_ready())
            return make_tool_result({{"error","Run run_pipeline first"}}, true);

        json result = json::array();
        std::unordered_set<std::string> all_tests;

        for (const auto& p : args["files"]) {
            std::string path = p.get<std::string>();
            auto it = ctx.graph.path_to_id.find(path);
            if (it == ctx.graph.path_to_id.end()) {
                result.push_back({{"file", path}, {"tests", json::array()}, {"indexed", false}});
                continue;
            }

            json tests = json::array();
            auto in = ctx.graph.incoming.find(it->second);
            if (in != ctx.graph.incoming.end()) {
                for (int64_t src_id : in->second) {
                    auto p2 = ctx.graph.id_to_path.find(src_id);
                    if (p2 == ctx.graph.id_to_path.end()) continue;
                    if (!is_test_path(p2->second)) continue;
                    tests.push_back(p2->second);
                    all_tests.insert(p2->second);
                }
            }
            // If target is itself a test file, include it
            if (is_test_path(path)) {
                tests.push_back(path);
                all_tests.insert(path);
            }

            std::sort(tests.begin(), tests.end());
            tests.erase(std::unique(tests.begin(), tests.end()), tests.end());
            result.push_back({{"file", path}, {"tests", tests}, {"indexed", true}});
        }

        return make_tool_result({
            {"per_file",     result},
            {"all_tests",    std::vector<std::string>(all_tests.begin(), all_tests.end())},
            {"total_tests",  (int)all_tests.size()},
            {"note", "Tests are detected by path convention (_test.*, *.spec.*, /tests/, etc) among files that import the target. Check get_skeleton(tests) to confirm coverage."}
        });
    }

    return make_tool_result({{"error", "Unknown tool: " + name}}, true);
}

void run_stdio(ServerContext& ctx) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        json req;
        try { req = json::parse(line); }
        catch (...) {
            std::cout << make_error(nullptr, PARSE_ERROR, "Parse error").dump() << "\n";
            std::cout.flush();
            continue;
        }

        std::string method = req.value("method", "");
        json id = req.contains("id") ? req["id"] : json(nullptr);

        if (id.is_null() && method != "initialize") continue;

        json response;

        if (method == "initialize") {
            response = make_response(id, {
                {"protocolVersion", "2024-11-05"},
                {"capabilities", {{"tools", {{"listChanged", false}}}}},
                {"serverInfo", {{"name", "axon"}, {"version", "0.1.0"}}}
            });
        } else if (method == "notifications/initialized") {
            continue;
        } else if (method == "tools/list") {
            response = make_response(id, tools_list());
        } else if (method == "tools/call") {
            auto params = req.value("params", json::object());
            std::string tool_name = params.value("name", "");
            json targs = params.value("arguments", json::object());
            try {
                response = make_response(id, handle_tool(tool_name, targs, ctx));
            } catch (const std::exception& e) {
                response = make_response(id, make_tool_result(
                    {{"error", std::string(e.what())}}, true));
            }
        } else {
            response = make_error(id, METHOD_NOT_FOUND, "Method not found: " + method);
        }

        std::cout << response.dump() << "\n";
        std::cout.flush();
    }
}

} // namespace axon::mcp
