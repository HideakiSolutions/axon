#include "server.hpp"
#include "protocol.hpp"
#include "version.hpp"
#include "../core/registry.hpp"
#include "../core/indexer.hpp"
#include "../core/capsule.hpp"
#include "../core/compress.hpp"
#include "../core/skeleton.hpp"
#include "../core/embeddings.hpp"
#include "../core/rename.hpp"
#include "../core/git.hpp"
#include "../core/routes.hpp"
#include "../core/dialogue.hpp"
#include "../core/telemetry.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <filesystem>
#include <unordered_set>
#include <queue>
#include <algorithm>
#include <cctype>
#include <chrono>

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
         {"description","Return token-efficient context: pivot files in full + support files skeletonized. Repeat queries against an unchanged index hit a server-side cache (~9x faster); pass no_cache=true or supply explicit pivot_files to bypass. Set dialogue_budget>0 to include relevant conversation turns anchored to the pivot files. Set compression=\"body\" to enable dense lossless-by-selection body compression (Balde A) instead of blind byte-truncation; default \"off\" is byte-identical to the previous behaviour."},
         {"inputSchema",{{"type","object"},{"properties",{
             {"query",{{"type","string"}}},
             {"pivot_files",{{"type","array"},{"items",{{"type","string"}}}}},
             {"token_budget",{{"type","integer"},{"default",8000}}},
             {"dialogue_budget",{{"type","integer"},{"default",0}}},
             {"no_cache",{{"type","boolean"},{"default",false}}},
             {"compression",{{"type","string"},{"enum",{"off","body"}},{"description","\"off\" (default) = current behaviour; \"body\" = dense compression preserving declarations and control-flow, eliding the rest"}}}}}}}},
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
        {{"name","rename"},
         {"description","Graph-assisted rename: find all occurrences of a symbol name across the codebase and return line-level edits. With dry_run=false, writes files to disk."},
         {"inputSchema",{{"type","object"},{"required",{"symbol_name","new_name"}},{"properties",{
             {"symbol_name",{{"type","string"},{"description","Current name of the symbol to rename"}}},
             {"new_name",   {{"type","string"},{"description","New name for the symbol"}}},
             {"dry_run",    {{"type","boolean"},{"default",true},{"description","If true, return edits without writing to disk"}}}}}}}},
        {{"name","route_map"},
         {"description","List all detected HTTP routes with their handler files and framework. Requires index_routes=true in .axon/config.toml."},
         {"inputSchema",{{"type","object"},{"properties",{
             {"framework",{{"type","string"},{"description","Filter by framework: nextjs|express|fastapi|flask"}}}}}}}},
        {{"name","api_impact"},
         {"description","Given a route path, return its handler file and the full impact graph of files that depend on the handler."},
         {"inputSchema",{{"type","object"},{"required",{"route_path"}},{"properties",{
             {"route_path",{{"type","string"},{"description","Route path to analyze, e.g. /api/users/:id"}}}}}}}},
        {{"name","detect_changes"},
         {"description","Detect which symbols and files are affected by recent git changes. Returns changed files, affected symbols (those whose line ranges overlap with the diff hunks), and impacted downstream files."},
         {"inputSchema",{{"type","object"},{"properties",{
             {"ref",{{"type","string"},{"default","HEAD"},{"description","Git ref to diff against, e.g. HEAD~1, main, a commit SHA"}}}}}}}},
        {{"name","group_list"},
         {"description","List all repos registered in the global Axon registry (~/.axon/registry.json) and their groups."},
         {"inputSchema",{{"type","object"},{"properties",json::object()}}}},
        {{"name","group_impact"},
         {"description","Cross-repo blast radius: given a file path in the current repo, return impacted files in other registered repos that import the same module path."},
         {"inputSchema",{{"type","object"},{"required",{"file"}},{"properties",{
             {"file",{{"type","string"},{"description","Relative or absolute path to the file to analyze"}}},
             {"group",{{"type","string"},{"description","Optional: limit to repos in this group"}}}}}}}},
        // ── Dialogue Layer ─────────────────────────────────────────────────────
        {{"name","thread_create"},
         {"description","Create a named conversation thread. kind: project|person|topic."},
         {"inputSchema",{{"type","object"},{"required",{"name"}},{"properties",{
             {"name",{{"type","string"}}},
             {"kind",{{"type","string"},{"default","project"}}}}}}}},
        {{"name","thread_list"},
         {"description","List all conversation threads with session counts."},
         {"inputSchema",{{"type","object"},{"properties",json::object()}}}},
        {{"name","session_start"},
         {"description","Start a new session within a thread. Returns the session id."},
         {"inputSchema",{{"type","object"},{"required",{"thread_id"}},{"properties",{
             {"thread_id",{{"type","integer"}}},
             {"label",{{"type","string"}}}}}}}},
        {{"name","session_end"},
         {"description","Close a session and optionally compute its digest (compressed summary)."},
         {"inputSchema",{{"type","object"},{"required",{"session_id"}},{"properties",{
             {"session_id",{{"type","integer"}}},
             {"compute_digest",{{"type","boolean"},{"default",true}}}}}}}},
        {{"name","turn_add"},
         {"description","Append a verbatim turn to a session. Automatically detects and anchors file paths and symbol names found in the content."},
         {"inputSchema",{{"type","object"},{"required",{"session_id","role","content"}},{"properties",{
             {"session_id",{{"type","integer"}}},
             {"role",{{"type","string"},{"enum",{"user","assistant"}}}},
             {"content",{{"type","string"}}}}}}}},
        {{"name","turn_search"},
         {"description","Semantic search over conversation turns. Returns the most relevant turns with session and thread context."},
         {"inputSchema",{{"type","object"},{"required",{"query"}},{"properties",{
             {"query",{{"type","string"}}},
             {"limit",{{"type","integer"},{"default",5}}},
             {"thread_id",{{"type","integer"},{"description","Scope to a specific thread. Omit for global search."}}}}}}}},
        {{"name","session_get"},
         {"description","Get all turns in a session, ordered by time."},
         {"inputSchema",{{"type","object"},{"required",{"session_id"}},{"properties",{
             {"session_id",{{"type","integer"}}},
             {"limit",{{"type","integer"},{"default",500}}}}}}}},
        {{"name","thread_get"},
         {"description","Get all sessions in a thread, with digests."},
         {"inputSchema",{{"type","object"},{"required",{"thread_id"}},{"properties",{
             {"thread_id",{{"type","integer"}}}}}}}},
        {{"name","anchor_link"},
         {"description","Manually link a turn to a file or symbol in the project graph."},
         {"inputSchema",{{"type","object"},{"required",{"turn_id"}},{"properties",{
             {"turn_id",{{"type","integer"}}},
             {"file_id",{{"type","integer"}}},
             {"symbol_id",{{"type","integer"}}},
             {"kind",{{"type","string"},{"default","mentions"}}}}}}}},
        {{"name","dialogue_context"},
         {"description","Return conversation turns relevant to a query or set of files. Use to surface past decisions and discussions before editing code."},
         {"inputSchema",{{"type","object"},{"required",{"query"}},{"properties",{
             {"query",{{"type","string"}}},
             {"file_paths",{{"type","array"},{"items",{{"type","string"}}}}},
             {"limit",{{"type","integer"},{"default",5}}},
             {"thread_id",{{"type","integer"}}}}}}}}
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
        try {
            embed_pending_symbols(*ctx.db, *ctx.model);
            embed_pending_turns(*ctx.db, *ctx.model);
        }
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
        try {
            embed_pending_symbols(*ctx.db, *ctx.model);
            embed_pending_turns(*ctx.db, *ctx.model);
        }
        catch (...) { /* silent — will retry next drain */ }
    }
    if (stats.files_indexed > 0 || stats.files_pruned > 0)
        ctx.graph = load_graph(*ctx.db);
}

static bool ensure_db_open(ServerContext& ctx, bool create_if_missing = false) {
    namespace fs = std::filesystem;
    if (ctx.db_ready()) return true;

    ctx.db_error.clear();
    if (!create_if_missing && !fs::exists(ctx.cfg.db_path)) {
        ctx.db_error = "No Axon index found at " + ctx.cfg.db_path.string() +
                       ". Run run_pipeline first.";
        return false;
    }

    try {
        fs::create_directories(ctx.cfg.axon_dir);
        ctx.db = std::make_unique<Database>(ctx.cfg.db_path);
        ctx.graph = load_graph(*ctx.db);
        if (!ctx.model_ready()) {
            try {
                fs::path binary_dir = ctx.binary_dir.empty()
                    ? ctx.cfg.project_root / "models"
                    : ctx.binary_dir;
                auto model_path = find_model(binary_dir);
                ctx.model = std::make_unique<EmbeddingModel>(model_path);
            } catch (...) {
                // The model is optional for startup; tools that need embeddings
                // already return an explicit error when it is unavailable.
            }
        }
        return true;
    } catch (const std::exception& e) {
        ctx.db.reset();
        ctx.graph = DependencyGraph{};
        ctx.db_error = e.what();
        return false;
    }
}

static json db_unavailable_result(const ServerContext& ctx) {
    std::string detail = ctx.db_error.empty()
        ? "Axon index database is not initialized."
        : ctx.db_error;

    return make_tool_result({
        {"error", "Axon index database unavailable"},
        {"db_path", ctx.cfg.db_path.string()},
        {"detail", detail},
        {"hint", "If another Claude/Codex session is open in this project, close it or stop the older axon serve process, then retry this tool. If no index exists yet, call run_pipeline."}
    }, true);
}

static json handle_tool(const std::string& name, const json& args, ServerContext& ctx) {
    ensure_db_open(ctx, name == "run_pipeline");

    // Order matters: sync first (full walk detects mv/rm/new-file side-effects),
    // then drain pending-writes (re-resolves edges for files that already have
    // peers in the DB after sync). Reversed order loses edges when a Write and
    // a Bash mv/rename happen in the same turn — drain would process the edited
    // file before sync has inserted the new peer, so resolve_edges misses.
    maybe_run_sync(ctx);
    drain_pending_writes(ctx);

    if (name == "run_pipeline") {
        if (!ctx.db_ready())
            return db_unavailable_result(ctx);

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

        int sym_embedded = 0, turn_embedded = 0;
        try {
            sym_embedded  = embed_pending_symbols(*ctx.db, *ctx.model);
            turn_embedded = embed_pending_turns(*ctx.db, *ctx.model);
        } catch (...) { /* silent — will retry on next drain */ }

        return make_tool_result({
            {"files_indexed",   stats.files_indexed},
            {"symbols_found",   stats.symbols_found},
            {"edges_found",     stats.edges_found},
            {"symbols_embedded", sym_embedded},
            {"turns_embedded",   turn_embedded}
        });
    }

    if (name == "index_paths") {
        if (!ctx.db_ready())
            return db_unavailable_result(ctx);

        std::vector<std::filesystem::path> paths;
        if (args.contains("paths"))
            for (const auto& p : args["paths"]) paths.emplace_back(p.get<std::string>());
        bool prune = args.value("prune", false);

        auto stats = index_files(ctx.cfg, *ctx.db, paths, prune);

        // If we inserted new symbols and a model is available, embed them inline
        // so the next semantic query already sees them.
        int embedded = 0, turns_embedded = 0;
        if (stats.files_indexed > 0 && ctx.model_ready()) {
            try {
                embedded       = embed_pending_symbols(*ctx.db, *ctx.model);
                turns_embedded = embed_pending_turns(*ctx.db, *ctx.model);
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
            {"symbols_embedded", embedded},
            {"turns_embedded",   turns_embedded}
        });
    }

    if (name == "get_context_capsule") {
        if (!ctx.db_ready())
            return db_unavailable_result(ctx);

        std::string query = args.value("query", "");
        int budget = args.value("token_budget", 8000);
        int dialogue_budget = args.value("dialogue_budget", 0);
        bool no_cache = args.value("no_cache", false);
        std::vector<std::string> pivots;
        if (args.contains("pivot_files"))
            for (const auto& p : args["pivot_files"]) pivots.push_back(p.get<std::string>());

        // compression: caller arg wins; fall back to project config default.
        std::string compression_str = args.value("compression",
                                                 ctx.cfg.project_cfg.capsule_compression);
        CapsuleCompression compression = compression_from_string(compression_str);

        // Cache lookup is keyed by (query, budget, project_epoch). Pivots are
        // intentionally NOT in the key — different pivot sets steer the
        // assembly but the same query/budget against the same epoch can
        // legitimately reuse the cached payload when the caller doesn't
        // override pivots. When pivots are explicit, skip cache to avoid
        // returning a cache entry generated for the implicit-pivot path.
        // Dialogue budget also bypasses cache (turns change independently of code index).
        // Body compression produces a different output than Off — bypass cache to
        // avoid serving a non-compressed entry under a compressed request or vice versa.
        const std::string epoch = current_project_epoch(*ctx.db);
        const bool eligible_for_cache = !no_cache && pivots.empty()
                                        && dialogue_budget == 0
                                        && compression == CapsuleCompression::Off;
        std::string cache_key;
        if (eligible_for_cache) {
            cache_key = compute_capsule_cache_key(query, budget, epoch);
            if (auto hit = capsule_cache_lookup(*ctx.db, cache_key, epoch)) {
                json pf = json::array();
                for (const auto& f : hit->pivot_files)
                    pf.push_back({{"path",f.path},{"content",f.content},{"tokens",f.token_estimate}});
                json sf = json::array();
                for (const auto& f : hit->support_files)
                    sf.push_back({{"path",f.path},{"content",f.content},{"tokens",f.token_estimate}});
                return make_tool_result({
                    {"query", hit->query},
                    {"pivot_files", pf},
                    {"support_files", sf},
                    {"related_turns", json::array()},
                    {"token_estimate", hit->token_estimate},
                    {"total_files_indexed", hit->total_files},
                    {"cache", "hit"}
                });
            }
        }

        // Miss path needs the embedding model; defer the readiness check until
        // here so cache hits don't require it.
        if (!ctx.model_ready())
            return make_tool_result({{"error","Embedding model not loaded; run_pipeline first"}}, true);

        auto capsule = assemble_capsule(query, pivots, *ctx.db, *ctx.model,
                                        ctx.graph, ctx.cfg.project_root, budget,
                                        dialogue_budget, compression);

        if (eligible_for_cache) {
            capsule_cache_insert(*ctx.db, cache_key, epoch, capsule);
        }

        json pf = json::array();
        for (const auto& f : capsule.pivot_files)
            pf.push_back({{"path",f.path},{"content",f.content},{"tokens",f.token_estimate}});
        json sf = json::array();
        for (const auto& f : capsule.support_files)
            sf.push_back({{"path",f.path},{"content",f.content},{"tokens",f.token_estimate}});
        json rt = json::array();
        for (const auto& t : capsule.related_turns)
            rt.push_back({{"role",t.role},{"content",t.content},
                          {"session",t.session_label},{"thread",t.thread_name},
                          {"ts",t.ts},{"tokens",t.token_estimate}});

        return make_tool_result({
            {"query", capsule.query},
            {"pivot_files", pf},
            {"support_files", sf},
            {"related_turns", rt},
            {"token_estimate", capsule.token_estimate},
            {"total_files_indexed", capsule.total_files},
            {"cache", "miss"}
        });
    }

    if (name == "get_impact_graph") {
        if (!ctx.db_ready())
            return db_unavailable_result(ctx);

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
            return db_unavailable_result(ctx);

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
                else if (ls == "bash")       lang = Language::Bash;
                else if (ls == "cpp")        lang = Language::Cpp;
                else if (ls == "kotlin")     lang = Language::Kotlin;
                else if (ls == "vue")        lang = Language::Vue;
                else if (ls == "lua")        lang = Language::Lua;
                else if (ls == "nix")        lang = Language::Nix;
                else if (ls == "ruby")       lang = Language::Ruby;
                else if (ls == "swift")      lang = Language::Swift;
                else if (ls == "scala")      lang = Language::Scala;

                result.push_back({{"path", path}, {"skeleton", skeletonize(content, lang)}});
            }
        }
        return make_tool_result(result);
    }

    if (name == "search_memory") {
        if (!ctx.db_ready())
            return db_unavailable_result(ctx);
        if (!ctx.model_ready())
            return make_tool_result({{"error","Embedding model not loaded; run_pipeline first"}}, true);

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
            return db_unavailable_result(ctx);

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
            return db_unavailable_result(ctx);

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
            return db_unavailable_result(ctx);

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
            return db_unavailable_result(ctx);

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

    if (name == "rename") {
        if (!ctx.db_ready())
            return db_unavailable_result(ctx);

        std::string old_name = args.value("symbol_name", "");
        std::string new_name = args.value("new_name", "");
        bool dry_run = args.value("dry_run", true);

        if (old_name.empty() || new_name.empty())
            return make_tool_result({{"error","symbol_name and new_name are required"}}, true);
        if (old_name == new_name)
            return make_tool_result({{"error","old and new name are the same"}}, true);

        // Find all files containing the symbol (via DB symbols table + caller files)
        std::unordered_set<std::string> candidate_files;

        // Files that define the symbol
        auto def_res = ctx.db->conn().Query(
            "SELECT DISTINCT f.path FROM symbols s JOIN files f ON s.file_id = f.id "
            "WHERE s.name = '" + sql_escape(old_name) + "'");
        if (!def_res->HasError()) {
            for (duckdb::idx_t i = 0; i < def_res->RowCount(); i++) {
                std::string fpath = def_res->GetValue(0, i).ToString();
                candidate_files.insert(fpath);

                // Also add files that import the defining file (callers)
                auto fid_res = ctx.db->conn().Query(
                    "SELECT id FROM files WHERE path = '" + sql_escape(fpath) + "'");
                if (!fid_res->HasError() && fid_res->RowCount() > 0) {
                    int64_t fid = fid_res->GetValue<int64_t>(0, 0);
                    auto it = ctx.graph.incoming.find(fid);
                    if (it != ctx.graph.incoming.end())
                        for (int64_t src : it->second) {
                            auto pit = ctx.graph.id_to_path.find(src);
                            if (pit != ctx.graph.id_to_path.end())
                                candidate_files.insert(pit->second);
                        }
                }
            }
        }

        if (candidate_files.empty())
            return make_tool_result({
                {"symbol_name", old_name},
                {"new_name",    new_name},
                {"edits",       json::array()},
                {"note",        "Symbol not found in index. Run run_pipeline first or check the name."}
            });

        // Collect edits from all candidate files
        std::vector<axon::RenameEdit> all_edits;
        std::string root = ctx.cfg.project_root.string();
        for (const auto& rel_path : candidate_files) {
            std::string abs_path = root + "/" + rel_path;
            auto file_edits = axon::collect_rename_edits(abs_path, old_name, new_name);
            for (auto& e : file_edits) {
                e.file_path = rel_path;  // store relative path in result
                all_edits.push_back(e);
            }
        }

        json edits_json = json::array();
        for (const auto& e : all_edits) {
            edits_json.push_back({
                {"file", e.file_path},
                {"line", e.line},
                {"old",  e.old_text},
                {"new",  e.new_text}
            });
        }

        int files_written = 0;
        if (!dry_run && !all_edits.empty()) {
            // Restore absolute paths for writing
            std::vector<axon::RenameEdit> abs_edits = all_edits;
            for (auto& e : abs_edits) e.file_path = root + "/" + e.file_path;
            files_written = axon::apply_rename_edits(abs_edits);
        }

        return make_tool_result({
            {"symbol_name",    old_name},
            {"new_name",       new_name},
            {"dry_run",        dry_run},
            {"edits",          edits_json},
            {"total_edits",    (int)all_edits.size()},
            {"files_affected", (int)candidate_files.size()},
            {"files_written",  files_written},
            {"note", dry_run ? "Dry run — no files written. Set dry_run=false to apply." : "Changes applied to disk. Run index_paths to update the index."}
        });
    }

    if (name == "detect_changes") {
        if (!ctx.db_ready())
            return db_unavailable_result(ctx);

        std::string ref = args.value("ref", "HEAD");
        std::string root = ctx.cfg.project_root.string();

        if (!axon::is_git_repo(root))
            return make_tool_result({{"error","Not a git repository"}}, true);

        auto diffs = axon::get_git_diffs(root, ref);
        if (diffs.empty())
            return make_tool_result({
                {"ref", ref},
                {"changed_files", json::array()},
                {"affected_symbols", json::array()},
                {"impacted_files", json::array()},
                {"note", "No changes detected for ref: " + ref}
            });

        json changed_files = json::array();
        json affected_symbols = json::array();
        std::unordered_set<int64_t> impacted_file_ids;

        for (const auto& diff : diffs) {
            changed_files.push_back(diff.path);
            if (diff.hunks.empty()) continue;

            auto fid_res = ctx.db->conn().Query(
                "SELECT id FROM files WHERE path = '" + sql_escape(diff.path) + "'");
            if (fid_res->HasError() || fid_res->RowCount() == 0) continue;
            int64_t file_id = fid_res->GetValue<int64_t>(0, 0);

            for (const auto& hunk : diff.hunks) {
                auto sym_res = ctx.db->conn().Query(
                    "SELECT s.name, s.kind, s.start_line, s.end_line, s.signature "
                    "FROM symbols s WHERE s.file_id = " + std::to_string(file_id) +
                    " AND s.start_line <= " + std::to_string(hunk.end_line) +
                    " AND s.end_line   >= " + std::to_string(hunk.start_line));
                if (sym_res->HasError()) continue;
                auto& sr = *sym_res;
                for (duckdb::idx_t i = 0; i < sr.RowCount(); i++) {
                    affected_symbols.push_back({
                        {"name",       sr.GetValue(0, i).ToString()},
                        {"kind",       sr.GetValue(1, i).ToString()},
                        {"file",       diff.path},
                        {"start_line", sr.GetValue(2, i).GetValue<int32_t>()},
                        {"end_line",   sr.GetValue(3, i).GetValue<int32_t>()},
                        {"signature",  sr.GetValue(4, i).ToString()}
                    });
                }
            }

            impacted_file_ids.insert(file_id);
            auto it = ctx.graph.incoming.find(file_id);
            if (it != ctx.graph.incoming.end())
                for (int64_t src : it->second) impacted_file_ids.insert(src);
        }

        std::unordered_set<std::string> changed_set(changed_files.begin(), changed_files.end());
        json impacted = json::array();
        for (int64_t fid : impacted_file_ids) {
            auto pit = ctx.graph.id_to_path.find(fid);
            if (pit != ctx.graph.id_to_path.end() && !changed_set.count(pit->second))
                impacted.push_back(pit->second);
        }

        return make_tool_result({
            {"ref",              ref},
            {"changed_files",    changed_files},
            {"affected_symbols", affected_symbols},
            {"impacted_files",   impacted},
            {"summary", std::to_string(changed_files.size()) + " files changed, " +
                        std::to_string(affected_symbols.size()) + " symbols affected, " +
                        std::to_string(impacted.size()) + " files impacted"}
        });
    }

    if (name == "route_map") {
        if (!ctx.db_ready())
            return db_unavailable_result(ctx);

        // Trigger route indexing if not done (or if index_routes enabled)
        if (ctx.cfg.project_cfg.index_routes) {
            try { axon::index_routes(ctx.cfg, *ctx.db); } catch (...) {}
        }

        std::string framework_filter = args.value("framework", "");
        auto routes = axon::get_all_routes(*ctx.db);

        json routes_json = json::array();
        for (const auto& r : routes) {
            if (!framework_filter.empty() && r.framework != framework_filter) continue;
            routes_json.push_back({
                {"method",       r.method},
                {"path",         r.path},
                {"handler_file", r.handler_file},
                {"framework",    r.framework}
            });
        }
        return make_tool_result({
            {"routes", routes_json},
            {"total",  (int)routes_json.size()},
            {"note", routes_json.empty()
                ? "No routes found. Set index_routes=true in .axon/config.toml and run run_pipeline."
                : ""}
        });
    }

    if (name == "api_impact") {
        if (!ctx.db_ready())
            return db_unavailable_result(ctx);

        std::string route_path = args.value("route_path", "");
        if (route_path.empty())
            return make_tool_result({{"error","route_path is required"}}, true);

        // Find route in DB
        auto rr = ctx.db->conn().Query(
            "SELECT method, path, handler_file, framework, file_id "
            "FROM routes WHERE path = '" + sql_escape(route_path) + "' LIMIT 1");
        if (rr->HasError() || rr->RowCount() == 0)
            return make_tool_result({
                {"error", "Route not found: " + route_path},
                {"hint", "Run route_map to list available routes, or enable index_routes in config."}
            }, true);

        std::string handler_file = rr->GetValue(2, 0).ToString();
        int64_t file_id = rr->GetValue<int64_t>(4, 0);

        // Collect files impacted by changes to this handler
        json impacted = json::array();
        std::unordered_set<int64_t> visited;
        std::queue<int64_t> q;
        q.push(file_id);
        while (!q.empty()) {
            int64_t fid = q.front(); q.pop();
            if (!visited.insert(fid).second) continue;
            auto it = ctx.graph.incoming.find(fid);
            if (it != ctx.graph.incoming.end())
                for (int64_t src : it->second) q.push(src);
        }
        visited.erase(file_id);  // exclude the handler itself
        for (int64_t fid : visited) {
            auto pit = ctx.graph.id_to_path.find(fid);
            if (pit != ctx.graph.id_to_path.end())
                impacted.push_back(pit->second);
        }

        return make_tool_result({
            {"route",        {{"method", rr->GetValue(0,0).ToString()},
                              {"path",   route_path},
                              {"framework", rr->GetValue(3,0).ToString()}}},
            {"handler_file", handler_file},
            {"impacted_files", impacted},
            {"impacted_count", (int)impacted.size()}
        });
    }

    if (name == "group_list") {
        auto reg = axon::load_registry();
        json repos_arr = json::array();
        for (auto& r : reg.repos) {
            repos_arr.push_back({{"name", r.name}, {"root", r.root}, {"db_path", r.db_path}});
        }
        json groups_arr = json::array();
        for (auto& [gname, members] : reg.groups) {
            groups_arr.push_back({{"name", gname}, {"repos", members}});
        }
        return make_tool_result({{"repos", repos_arr}, {"groups", groups_arr}});
    }

    if (name == "group_impact") {
        std::string file_arg = args.value("file", "");
        if (file_arg.empty())
            return make_tool_result({{"error","file is required"}}, true);

        std::string group_filter = args.value("group", "");
        auto reg = axon::load_registry();

        // Determine the stem of the target file for cross-repo matching
        std::filesystem::path file_path(file_arg);
        std::string stem = file_path.stem().string();

        // Determine repos to scan
        std::vector<axon::RepoEntry> repos_to_scan;
        if (!group_filter.empty()) {
            repos_to_scan = axon::get_group_repos(reg, group_filter);
        } else {
            repos_to_scan = axon::get_repos(reg);
        }

        // Exclude the current repo
        std::string current_root = ctx.cfg.project_root.string();
        json cross_impact = json::array();

        for (const auto& r : repos_to_scan) {
            if (r.root == current_root) continue;
            if (!std::filesystem::exists(r.db_path)) continue;

            try {
                duckdb::DuckDB other_db(r.db_path);
                duckdb::Connection other_conn(other_db);

                std::string sql =
                    "SELECT DISTINCT f.path FROM files f "
                    "JOIN edges e ON e.from_id = f.id "
                    "JOIN files f2 ON e.to_id = f2.id "
                    "WHERE f2.path LIKE '%" + sql_escape(stem) + "%' LIMIT 50";

                auto res = other_conn.Query(sql);
                if (res->HasError()) continue;

                json impacted = json::array();
                for (duckdb::idx_t i = 0; i < res->RowCount(); i++) {
                    impacted.push_back(res->GetValue(0, i).ToString());
                }
                cross_impact.push_back({
                    {"repo",           r.name},
                    {"repo_root",      r.root},
                    {"impacted_files", impacted}
                });
            } catch (...) {
                // Skip repos with inaccessible/corrupt DBs
            }
        }

        return make_tool_result({
            {"file",         file_arg},
            {"stem",         stem},
            {"group_filter", group_filter},
            {"cross_repo_impact", cross_impact}
        });
    }

    // ── Dialogue Layer tools ──────────────────────────────────────────────────

    if (name == "thread_create") {
        if (!ctx.db_ready()) return db_unavailable_result(ctx);
        std::string tname = args.value("name", "");
        std::string kind  = args.value("kind", "project");
        if (tname.empty())
            return make_tool_result({{"error","name is required"}}, true);
        int64_t id = thread_create(*ctx.db, tname, kind);
        return make_tool_result({{"id", id}, {"name", tname}, {"kind", kind}});
    }

    if (name == "thread_list") {
        if (!ctx.db_ready()) return db_unavailable_result(ctx);
        auto threads = thread_list(*ctx.db);
        json result = json::array();
        for (const auto& t : threads)
            result.push_back({{"id",t.id},{"name",t.name},{"kind",t.kind},{"created_at",t.created_at}});
        return make_tool_result(result);
    }

    if (name == "session_start") {
        if (!ctx.db_ready()) return db_unavailable_result(ctx);
        int64_t thread_id = args.value("thread_id", int64_t(-1));
        std::string label = args.value("label", "");
        if (thread_id < 0)
            return make_tool_result({{"error","thread_id is required"}}, true);
        int64_t id = session_start(*ctx.db, thread_id, label);
        return make_tool_result({{"session_id", id}, {"thread_id", thread_id}, {"label", label}});
    }

    if (name == "session_end") {
        if (!ctx.db_ready()) return db_unavailable_result(ctx);
        int64_t session_id = args.value("session_id", int64_t(-1));
        bool compute_digest = args.value("compute_digest", true);
        if (session_id < 0)
            return make_tool_result({{"error","session_id is required"}}, true);
        session_end(*ctx.db, session_id, ctx.model_ready() ? ctx.model.get() : nullptr, compute_digest);
        return make_tool_result({{"session_id", session_id}, {"ended", true}});
    }

    if (name == "turn_add") {
        if (!ctx.db_ready()) return db_unavailable_result(ctx);
        int64_t session_id = args.value("session_id", int64_t(-1));
        std::string role    = args.value("role", "user");
        std::string content = args.value("content", "");
        if (session_id < 0 || content.empty())
            return make_tool_result({{"error","session_id and content are required"}}, true);
        int64_t id = turn_add(*ctx.db, ctx.model_ready() ? ctx.model.get() : nullptr,
                              session_id, role, content);
        return make_tool_result({{"turn_id", id}, {"session_id", session_id}, {"role", role}});
    }

    if (name == "turn_search") {
        if (!ctx.db_ready()) return db_unavailable_result(ctx);
        if (!ctx.model_ready())
            return make_tool_result({{"error","Embedding model not loaded; run_pipeline first"}}, true);
        std::string query = args.value("query", "");
        int limit         = args.value("limit", 5);
        int64_t thread_id = args.value("thread_id", int64_t(-1));
        auto hits = turn_search(*ctx.db, *ctx.model, query, limit, thread_id);
        json result = json::array();
        for (const auto& h : hits)
            result.push_back({{"turn_id",h.turn.id},{"role",h.turn.role},
                              {"content",h.turn.content},{"ts",h.turn.ts},
                              {"session",h.session_label},{"thread",h.thread_name},
                              {"score",h.score}});
        return make_tool_result(result);
    }

    if (name == "session_get") {
        if (!ctx.db_ready()) return db_unavailable_result(ctx);
        int64_t session_id = args.value("session_id", int64_t(-1));
        int limit          = args.value("limit", 500);
        if (session_id < 0)
            return make_tool_result({{"error","session_id is required"}}, true);
        auto turns = session_get(*ctx.db, session_id, limit);
        json result = json::array();
        for (const auto& t : turns)
            result.push_back({{"id",t.id},{"role",t.role},{"content",t.content},{"ts",t.ts}});
        return make_tool_result(result);
    }

    if (name == "thread_get") {
        if (!ctx.db_ready()) return db_unavailable_result(ctx);
        int64_t thread_id = args.value("thread_id", int64_t(-1));
        if (thread_id < 0)
            return make_tool_result({{"error","thread_id is required"}}, true);
        auto sessions = thread_get_sessions(*ctx.db, thread_id);
        json result = json::array();
        for (const auto& s : sessions)
            result.push_back({{"id",s.id},{"label",s.label},
                              {"started_at",s.started_at},{"ended_at",s.ended_at},
                              {"digest",s.digest}});
        return make_tool_result(result);
    }

    if (name == "anchor_link") {
        if (!ctx.db_ready()) return db_unavailable_result(ctx);
        int64_t turn_id   = args.value("turn_id",   int64_t(-1));
        int64_t file_id   = args.value("file_id",   int64_t(-1));
        int64_t symbol_id = args.value("symbol_id", int64_t(-1));
        std::string kind  = args.value("kind", "mentions");
        if (turn_id < 0)
            return make_tool_result({{"error","turn_id is required"}}, true);
        int64_t id = anchor_link(*ctx.db, turn_id, file_id, symbol_id, kind);
        return make_tool_result({{"anchor_id",id},{"turn_id",turn_id},
                                 {"file_id",file_id},{"symbol_id",symbol_id},{"kind",kind}});
    }

    if (name == "dialogue_context") {
        if (!ctx.db_ready()) return db_unavailable_result(ctx);
        if (!ctx.model_ready())
            return make_tool_result({{"error","Embedding model not loaded; run_pipeline first"}}, true);
        std::string query = args.value("query", "");
        int limit         = args.value("limit", 5);
        int64_t thread_id = args.value("thread_id", int64_t(-1));

        // Resolve optional file_paths to file_ids
        std::vector<int64_t> file_ids;
        if (args.contains("file_paths")) {
            for (const auto& fp : args["file_paths"]) {
                std::string path = fp.get<std::string>();
                auto res = ctx.db->conn().Query(
                    "SELECT id FROM files WHERE path LIKE '%" + sql_escape(path) + "' LIMIT 1");
                if (!res->HasError() && res->RowCount() > 0)
                    file_ids.push_back(res->GetValue<int64_t>(0, 0));
            }
        }

        std::vector<TurnHit> hits;
        if (!file_ids.empty()) {
            hits = turns_for_files(*ctx.db, *ctx.model, query, file_ids, limit * 300);
        } else {
            hits = turn_search(*ctx.db, *ctx.model, query, limit, thread_id);
        }

        json result = json::array();
        for (const auto& h : hits)
            result.push_back({{"turn_id",h.turn.id},{"role",h.turn.role},
                              {"content",h.turn.content},{"ts",h.turn.ts},
                              {"session",h.session_label},{"thread",h.thread_name},
                              {"score",h.score}});
        return make_tool_result(result);
    }

    return make_tool_result({{"error", "Unknown tool: " + name}}, true);
}

struct StdioEnvelope {
    bool framed = false;
    bool ok = false;
    json payload;
    json error;
};

static std::string strip_cr(std::string s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

static std::string trim_ascii(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

static std::string lower_ascii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool parse_header_line(const std::string& line, size_t& content_length) {
    auto colon = line.find(':');
    if (colon == std::string::npos) return false;

    std::string name = lower_ascii(trim_ascii(line.substr(0, colon)));
    std::string value = trim_ascii(line.substr(colon + 1));
    if (name != "content-length") return false;

    try {
        content_length = static_cast<size_t>(std::stoull(value));
        return true;
    } catch (...) {
        return false;
    }
}

static bool looks_like_header(const std::string& line) {
    if (line.empty()) return false;
    if (line.front() == '{' || line.front() == '[') return false;
    return line.find(':') != std::string::npos;
}

static bool read_stdio_envelope(std::istream& in, StdioEnvelope& env) {
    env = StdioEnvelope{};

    std::string first;
    while (std::getline(in, first)) {
        first = strip_cr(first);
        if (!first.empty()) break;
    }
    if (!in && first.empty()) return false;

    if (looks_like_header(first)) {
        env.framed = true;
        size_t content_length = 0;
        bool have_content_length = parse_header_line(first, content_length);

        std::string line;
        while (std::getline(in, line)) {
            line = strip_cr(line);
            if (line.empty()) break;
            size_t parsed_length = 0;
            if (parse_header_line(line, parsed_length)) {
                content_length = parsed_length;
                have_content_length = true;
            }
        }

        if (!have_content_length) {
            env.error = make_error(nullptr, INVALID_REQUEST, "Missing Content-Length header");
            return true;
        }

        std::string body(content_length, '\0');
        in.read(body.data(), static_cast<std::streamsize>(content_length));
        if (static_cast<size_t>(in.gcount()) != content_length) return false;

        try {
            env.payload = json::parse(body);
            env.ok = true;
        } catch (...) {
            env.error = make_error(nullptr, PARSE_ERROR, "Parse error");
        }
        return true;
    }

    try {
        env.payload = json::parse(first);
        env.ok = true;
    } catch (...) {
        env.error = make_error(nullptr, PARSE_ERROR, "Parse error");
    }
    return true;
}

static void write_stdio_response(const json& response, bool framed) {
    std::string body = response.dump();
    if (framed) {
        std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    } else {
        std::cout << body << "\n";
    }
    std::cout.flush();
}

void run_stdio(ServerContext& ctx) {
    StdioEnvelope env;
    while (read_stdio_envelope(std::cin, env)) {
        if (!env.ok) {
            write_stdio_response(env.error, env.framed);
            continue;
        }

        const json& req = env.payload;

        std::string method = req.value("method", "");
        json id = req.contains("id") ? req["id"] : json(nullptr);

        if (id.is_null() && method != "initialize") continue;

        json response;

        if (method == "initialize") {
            response = make_response(id, {
                {"protocolVersion", "2024-11-05"},
                {"capabilities", {{"tools", {{"listChanged", false}}}}},
                {"serverInfo", {{"name", "axon"}, {"version", axon::VERSION}}}
            });
        } else if (method == "notifications/initialized") {
            continue;
        } else if (method == "tools/list") {
            response = make_response(id, tools_list());
        } else if (method == "tools/call") {
            auto params = req.value("params", json::object());
            std::string tool_name = params.value("name", "");
            json targs = params.value("arguments", json::object());
            auto start = std::chrono::steady_clock::now();
            try {
                response = make_response(id, handle_tool(tool_name, targs, ctx));
            } catch (const std::exception& e) {
                response = make_response(id, make_tool_result(
                    {{"error", std::string(e.what())}}, true));
            }
            int64_t latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            std::string body = response.dump();
            int64_t tokens = static_cast<int64_t>(body.size() / 4);
            bool cache_hit = body.find("\\\"cache\\\": \\\"hit\\\"") != std::string::npos ||
                             body.find("\"cache\":\"hit\"") != std::string::npos;
            axon::record_telemetry(ctx.cfg, ctx.db.get(), {
                tool_name, "mcp", latency_ms, tokens, tokens * 4, tokens * 3, cache_hit
            });
        } else {
            response = make_error(id, METHOD_NOT_FOUND, "Method not found: " + method);
        }

        write_stdio_response(response, env.framed);
    }
}

} // namespace axon::mcp
