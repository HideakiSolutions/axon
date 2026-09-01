#include "core/config.hpp"
#include "core/registry.hpp"
#include "core/db.hpp"
#include "core/indexer.hpp"
#include "core/graph.hpp"
#include "core/skeleton.hpp"
#include "core/capsule.hpp"
#include "core/ccr.hpp"
#include "core/embeddings.hpp"
#include "core/shell_filter.hpp"
#include "core/telemetry.hpp"
#include "core/watcher.hpp"
#include "mcp/server.hpp"
#include "mcp/http_server.hpp"
#include "mcp/peer.hpp"
#include "lsp/server.hpp"
#include "portfolio/delivery/portfolio_capability_catalog.hpp"
#include "version.hpp"
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <thread>
#include <unordered_map>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;

static std::atomic<bool> g_watch_running{true};

static void stop_watch(int) {
    g_watch_running = false;
}

static int64_t elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}

static int estimate_tokens_cli(const std::string& s) {
    return static_cast<int>((s.size() + 3) / 4);
}

static void print_usage() {
    std::cerr << R"(
axon — Context Engine for AI Coding Agents

Usage:
  axon init   [path]                    Initialize .axon/config.toml with defaults
  axon index  [path] [--force]          Index project (parse + graph + embeddings)
  axon index-paths <files...> [--prune] Incrementally reindex specific files
                                        (--prune alone = only sweep deleted files)
  axon serve  [--http] [--port=7070] [--host=127.0.0.1] [--group=<name>] [--all]
                                        Start MCP server (stdio default; --http for REST API)
  axon web    [--port=7070] [--host=127.0.0.1] [--group=<name>] [--all]
                                        Start browser graph explorer + REST API
  axon lsp                              Start Language Server Protocol server (stdio)
  axon watch [path] [--interval-ms=1000] [--debounce-ms=500] [--backend=auto|native|poll]
                                        Watch for external edits and incrementally reindex
                                        (native inotify/FSEvents with automatic poll fallback)
  axon capsule <query> [--no-cache]     Print context capsule for a query
  axon artifact-retrieve <artifact_id>   Retrieve original CCR artifact content
  axon filter <kind> [--budget=N] [--metrics=json]
                                        Filter stdin output (auto|diff|lint|log|grep|json|package|test|tsc|text)
  axon skeleton <file>                  Print skeleton (signatures-only) of a file
  axon status                           Show index statistics
  axon metrics [--json]                 Show per-layer telemetry (token savings, latency)
  axon doctor locks [--json]            Diagnose registered DuckDB lock owners
  axon registry prune                   Drop registry entries whose repo root is gone
  axon portfolio <sync|reconcile|rebuild|status> [--group=<name>] [--json]
  axon capability <list|search|duplicates|compare|drift> ...
  axon help                             Show this help
  axon --version | -V                   Print version and git SHA

)";
}

static axon::Config load_config(const std::string& path_arg = "") {
    std::optional<fs::path> root;
    if (path_arg.empty()) {
        // No-arg: run from cwd, walk up to a project marker (unchanged).
        fs::path start = fs::current_path();
        root = axon::find_project_root(start);
        if (!root) root = start;
    } else {
        // Explicit path: that directory IS the root. Do NOT walk up — the
        // upward marker search is only meaningful for the no-arg case. Without
        // this, indexing a non-git dir whose ancestor happens to have a
        // ROOT_MARKER (stray package.json/CMakeLists.txt, e.g. under /tmp)
        // would index the ancestor instead of the dir the user asked for.
        root = fs::weakly_canonical(fs::path(path_arg));
    }
    return axon::make_config(*root);
}

static std::unique_ptr<axon::Database> open_database_or_report(const fs::path& db_path) {
    try {
        return std::make_unique<axon::Database>(db_path);
    } catch (const std::exception& e) {
        std::cerr << "[axon] " << axon::database_open_error_message(db_path, e) << "\n";
        return nullptr;
    }
}

static axon::mcp::ServerContext make_server_context(const char* binary_path,
                                                    bool load_model = true) {
    auto cfg = load_config();
    axon::mcp::ServerContext ctx;
    ctx.cfg = cfg;
    ctx.binary_dir = fs::path(binary_path).parent_path();

    axon::register_repo(cfg.project_root.string(), cfg.db_path.string());

    if (fs::exists(cfg.db_path)) {
        try {
            ctx.db = std::make_unique<axon::Database>(cfg.db_path);
            ctx.graph = axon::load_graph(*ctx.db);
            if (load_model) {
                try {
                    auto model_path = axon::find_model(ctx.binary_dir);
                    ctx.model = std::make_unique<axon::EmbeddingModel>(model_path);
                } catch (...) {
                }
            }
        } catch (const std::exception& e) {
            ctx.db_error = e.what();
        }
    }

    return ctx;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // The MSVC CRT opens stdin/stdout in text mode, which rewrites \n as
    // \r\n on write (and strips \r on read). That corrupts every byte-exact
    // surface: artifact-retrieve must reproduce the stored artifact
    // bit-for-bit, filter pipes arbitrary tool output, and the MCP/LSP
    // framing carries explicit \r\n headers of its own.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) {
        print_usage();
        return 1;
    }
    std::string cmd = argv[1];

    // ── axon --version | -V | version ─────────────────────────────────────
    if (cmd == "--version" || cmd == "-V" || cmd == "version") {
        std::cout << "axon " << axon::VERSION << " (build " << axon::GIT_SHA << ")\n";
        return 0;
    }

    // ── axon help | --help | -h ───────────────────────────────────────────
    // print_usage() writes to stderr (consistent with the error fall-through
    // at the end of main); the difference for help is the exit code — 0 here,
    // 1 for unknown command at the bottom.
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }

    // ── axon init [path] ──────────────────────────────────────────────────
    if (cmd == "init") {
        std::string path = argc > 2 ? argv[2] : "";
        auto cfg = load_config(path);
        auto config_path = cfg.axon_dir / "config.toml";
        if (fs::exists(config_path)) {
            std::cout << "Config already exists: " << config_path << "\n";
            return 0;
        }
        std::ofstream f(config_path);
        f << "# Axon project configuration\n"
             "# https://github.com/hideaki/axon\n\n"
             "# Granularity of dependency edges.\n"
             "# \"file\"   — edges connect files (default, faster indexing)\n"
             "# \"symbol\" — edges connect individual functions/classes (more precise callers)\n"
             "granularity = \"file\"\n\n"
             "# Set to true to detect and index HTTP routes (Next.js, Express, FastAPI, Django)\n"
             "index_routes = false\n\n"
             "# Enable full-text search index for symbol name lookup (BM25)\n"
             "fts_enabled = true\n\n"
             "# Default token budget for CLI capsules and config-backed MCP calls.\n"
             "token_budget = 8000\n\n"
             "# Capsule body compression: \"off\" (default) or \"body\".\n"
             "capsule_compression = \"off\"\n\n"
             "# Opt-in local telemetry. Can also be enabled with AXON_TELEMETRY=1.\n"
             "telemetry = false\n";
        std::cout << "Created " << config_path << "\n";
        return 0;
    }

    // ── axon index [path] [--force] ────────────────────────────────────────
    if (cmd == "index") {
        std::string path;
        bool force = false;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--force" || a == "-f")
                force = true;
            else if (path.empty())
                path = a;
        }
        if (!path.empty() && !fs::exists(path)) {
            std::cerr << "Error: path does not exist: " << path << "\n";
            return 1;
        }
        auto cfg = load_config(path);
        std::cout << "Indexing " << cfg.project_root << (force ? " (force)" : "") << " ...\n";

        auto db = open_database_or_report(cfg.db_path);
        if (!db) return 1;
        auto start = std::chrono::steady_clock::now();

        auto stats = axon::index_project(
            cfg, *db,
            [](const std::string& f, int done, int total) {
                std::cerr << "\r[" << done << "/" << total << "] " << f << "    ";
            },
            force);
        std::cerr << "\n";

        std::cout << "Done: " << stats.files_indexed << " files, " << stats.symbols_found
                  << " symbols, " << stats.edges_found << " edges\n";

        // Register this repo in the global registry
        axon::register_repo(cfg.project_root.string(), cfg.db_path.string());

        // Attempt to embed symbols if model is available
        try {
            auto model_path = axon::find_model(fs::path(argv[0]).parent_path());
            std::cout << "Loading embedding model...\n";
            axon::EmbeddingModel model(model_path);

            int n = axon::embed_pending_symbols(*db, model);
            if (n > 0) std::cout << "Embedded " << n << " symbols.\n";
        } catch (const std::exception& e) {
            std::cerr << "[warn] Skipping embeddings: " << e.what() << "\n";
            std::cerr << "       Run `axon index` again after downloading the model.\n";
        }
        axon::record_telemetry(cfg, db.get(),
                               {"index", "cli", elapsed_ms(start), stats.symbols_found * 12,
                                stats.files_indexed * 500, 0, false, "indexing"});
        return 0;
    }

    // ── axon index-paths <files...> [--prune] ──────────────────────────────
    if (cmd == "index-paths") {
        bool prune = false;
        std::vector<fs::path> paths;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--prune")
                prune = true;
            else
                paths.push_back(fs::path(a));
        }

        auto cfg = load_config();
        if (!fs::exists(cfg.db_path)) {
            std::cerr << "No index found. Run `axon index` first.\n";
            return 1;
        }

        auto db = open_database_or_report(cfg.db_path);
        if (!db) return 1;
        auto start = std::chrono::steady_clock::now();
        auto stats = axon::index_files(cfg, *db, paths, prune);

        // Embed any newly-inserted symbols so get_context_capsule sees them immediately
        if (stats.files_indexed > 0) {
            try {
                auto model_path = axon::find_model(fs::path(argv[0]).parent_path());
                axon::EmbeddingModel model(model_path);
                axon::embed_pending_symbols(*db, model);
            } catch (const std::exception& e) {
                std::cerr << "[warn] Skipping embeddings: " << e.what() << "\n";
            }
        }

        std::cout << stats.files_indexed << " indexed, " << stats.files_skipped << " unchanged";
        if (prune) std::cout << ", " << stats.files_pruned << " pruned";
        std::cout << "\n";
        axon::record_telemetry(cfg, db.get(),
                               {"index-paths", "cli", elapsed_ms(start), stats.symbols_found * 12,
                                stats.files_indexed * 500, 0, false, "indexing"});
        return 0;
    }

    // ── axon watch [path] [--interval-ms=N] [--debounce-ms=N] [--backend=B] ─
    if (cmd == "watch") {
        std::string path;
        int interval_ms = 1000;
        int debounce_ms = 500;
        axon::WatchBackend backend_pref = axon::WatchBackend::Auto;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a.rfind("--interval-ms=", 0) == 0)
                interval_ms = std::stoi(a.substr(14));
            else if (a.rfind("--debounce-ms=", 0) == 0)
                debounce_ms = std::stoi(a.substr(14));
            else if (a.rfind("--backend=", 0) == 0) {
                std::string b = a.substr(10);
                if (b == "native")
                    backend_pref = axon::WatchBackend::Native;
                else if (b == "poll")
                    backend_pref = axon::WatchBackend::Poll;
                else if (b != "auto") {
                    std::cerr << "Unknown --backend value: " << b << " (auto|native|poll)\n";
                    return 1;
                }
            } else if (path.empty())
                path = a;
        }
        if (interval_ms < 100) interval_ms = 100;
        if (debounce_ms < 0) debounce_ms = 0;

        auto cfg = load_config(path);
        if (!fs::exists(cfg.db_path)) {
            std::cerr << "No index found. Run `axon index` first.\n";
            return 1;
        }

        auto db = open_database_or_report(cfg.db_path);
        if (!db) return 1;
        signal(SIGINT, stop_watch);
#ifndef _WIN32
        signal(SIGTERM, stop_watch);
#endif

        std::string backend;
        auto watcher = axon::make_watcher(cfg, backend_pref, interval_ms, backend);
        if (!watcher) return 1;

        std::cout << "Watching " << cfg.project_root << " (backend " << backend << ", poll "
                  << interval_ms << "ms, debounce " << debounce_ms << "ms)\n";
        std::cout.flush();

        while (g_watch_running) {
            axon::WatchEvent ev;
            if (!watcher->wait_for_changes(std::chrono::milliseconds(interval_ms), ev)) continue;

            // Same debounce as the old poll loop, then drain whatever else
            // settled during the window so one save-burst is one reindex.
            std::this_thread::sleep_for(std::chrono::milliseconds(debounce_ms));
            axon::WatchEvent more;
            while (watcher->wait_for_changes(std::chrono::milliseconds(0), more)) {
                ev.merge(more);
                more = axon::WatchEvent{};
            }

            auto start = std::chrono::steady_clock::now();
            axon::IndexStats stats;
            if (ev.overflow) {
                // Kernel queue overflowed — events were lost; converge with a
                // full sync instead of trusting the partial batch.
                stats = axon::sync_project(cfg, *db);
                std::cout << "(event overflow — full rescan) ";
            } else {
                stats = axon::index_files(cfg, *db, ev.changed, ev.deleted);
            }
            std::cout << stats.files_indexed << " indexed, " << stats.files_skipped << " unchanged";
            if (ev.deleted || ev.overflow) std::cout << ", " << stats.files_pruned << " pruned";
            std::cout << "\n";
            std::cout.flush();
            axon::record_telemetry(cfg, db.get(),
                                   {"watch", "cli", elapsed_ms(start), stats.symbols_found * 12,
                                    stats.files_indexed * 500, 0, false, "indexing"});
        }
        watcher->stop();
        std::cout << "Watch stopped.\n";
        return 0;
    }

    // ── axon serve | axon web ──────────────────────────────────────────────
    if (cmd == "serve" || cmd == "web") {
        bool use_http = (cmd == "web");
        std::string http_host = "127.0.0.1";
        int http_port = 7070;
        std::string http_group;
        bool http_all_repos = false;

        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--http")
                use_http = true;
            else if (a.rfind("--port=", 0) == 0)
                http_port = std::stoi(a.substr(7));
            else if (a.rfind("--host=", 0) == 0)
                http_host = a.substr(7);
            else if (a.rfind("--group=", 0) == 0)
                http_group = a.substr(8);
            else if (a == "--all")
                http_all_repos = true;
        }

        // Hygiene advisory only — cleaning stays an explicit user action
        // (`axon registry prune`): auto-pruning at startup could drop the
        // registration of a temporarily unmounted root (decision 2026-07-09).
        if (int dead = axon::count_prunable(axon::load_registry()); dead > 0)
            std::cerr << "[axon] registry: " << dead << " dead entr" << (dead == 1 ? "y" : "ies")
                      << " (run 'axon registry prune' to clean)\n";

        auto ctx = make_server_context(argv[0]);

        if (use_http) {
            axon::mcp::HttpConfig http_cfg;
            http_cfg.host = http_host;
            http_cfg.port = http_port;
            http_cfg.group = http_group;
            http_cfg.all_repos = http_all_repos;
            axon::mcp::run_http(ctx, http_cfg);
        } else {
            axon::mcp::run_stdio(ctx);
        }
        return 0;
    }

    // ── axon lsp ───────────────────────────────────────────────────────────
    if (cmd == "lsp") {
        auto ctx = make_server_context(argv[0], false);
        axon::lsp::run_stdio(ctx);
        return 0;
    }

    // ── axon capsule <query> [--no-cache] ──────────────────────────────────
    if (cmd == "capsule") {
        if (argc < 3) {
            std::cerr << "Usage: axon capsule <query> [--no-cache]\n";
            return 1;
        }
        std::string query;
        bool no_cache = false;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--no-cache") {
                no_cache = true;
                continue;
            }
            if (!query.empty()) query += " ";
            query += a;
        }

        auto cfg = load_config();
        if (!fs::exists(cfg.db_path)) {
            std::cerr << "No index found. Run `axon index` first.\n";
            return 1;
        }

        auto db = open_database_or_report(cfg.db_path);
        if (!db) return 1;
        auto graph = axon::load_graph(*db);
        auto start = std::chrono::steady_clock::now();

        // Cache check (W2.T01) — skipped under --no-cache so devs can force
        // a fresh assemble after parser/grammar changes that would otherwise
        // be served from a stale entry.
        const std::string epoch = axon::current_project_epoch(*db);
        const std::string cache_key = axon::compute_capsule_cache_key(
            query, cfg.project_cfg.token_budget, epoch, axon::VERSION);
        if (!no_cache) {
            if (auto hit = axon::capsule_cache_lookup(*db, cache_key, epoch)) {
                std::cout << "{\n";
                std::cout << "  \"query\": \"" << hit->query << "\",\n";
                std::cout << "  \"token_estimate\": " << hit->token_estimate << ",\n";
                std::cout << "  \"pivot_files\": " << hit->pivot_files.size() << ",\n";
                std::cout << "  \"support_files\": " << hit->support_files.size() << ",\n";
                std::cout << "  \"compression_tokens_saved\": " << hit->compression_tokens_saved
                          << ",\n";
                std::cout << "  \"cache\": \"hit\"\n";
                std::cout << "}\n";
                for (const auto& f : hit->pivot_files)
                    std::cerr << "  [pivot]   " << f.path << " (" << f.token_estimate << " tok)\n";
                for (const auto& f : hit->support_files)
                    std::cerr << "  [support] " << f.path << " (" << f.token_estimate << " tok)\n";
                axon::record_telemetry(cfg, db.get(),
                                       {"capsule", "cli", elapsed_ms(start), hit->token_estimate,
                                        hit->token_estimate * 4, hit->token_estimate * 3, true,
                                        "cache"});
                return 0;
            }
        }

        std::optional<axon::EmbeddingModel> model_opt;
        try {
            auto model_path = axon::find_model(fs::path(argv[0]).parent_path());
            model_opt.emplace(model_path);
        } catch (const std::exception& e) {
            std::cerr << "[axon] " << e.what() << "\n";
            return 1;
        }
        axon::EmbeddingModel& model = *model_opt;

        auto compression = axon::compression_from_string(cfg.project_cfg.capsule_compression);
        auto capsule = axon::assemble_capsule(query, {}, *db, model, graph, cfg.project_root,
                                              cfg.project_cfg.token_budget, 0, compression);
        if (!no_cache) {
            axon::capsule_cache_insert(*db, cache_key, epoch, capsule);
        }
        if (capsule.compression_tokens_saved > 0) {
            axon::record_telemetry(cfg, db.get(),
                                   {"capsule.compression", "cli", 0,
                                    capsule.compression_output_tokens,
                                    capsule.compression_input_tokens,
                                    capsule.compression_tokens_saved, false, "compression"});
        }

        std::cout << "{\n";
        std::cout << "  \"query\": \"" << query << "\",\n";
        std::cout << "  \"token_estimate\": " << capsule.token_estimate << ",\n";
        std::cout << "  \"pivot_files\": " << capsule.pivot_files.size() << ",\n";
        std::cout << "  \"support_files\": " << capsule.support_files.size() << ",\n";
        std::cout << "  \"compression_tokens_saved\": " << capsule.compression_tokens_saved << "\n";
        std::cout << "}\n";

        for (const auto& f : capsule.pivot_files)
            std::cerr << "  [pivot]   " << f.path << " (" << f.token_estimate << " tok)\n";
        for (const auto& f : capsule.support_files)
            std::cerr << "  [support] " << f.path << " (" << f.token_estimate << " tok)\n";
        axon::record_telemetry(cfg, db.get(),
                               {"capsule", "cli", elapsed_ms(start), capsule.token_estimate,
                                capsule.token_estimate * 4, capsule.token_estimate * 3, false,
                                "retrieval"});
        return 0;
    }

    // ── axon artifact-retrieve <artifact_id> ───────────────────────────────
    if (cmd == "artifact-retrieve") {
        if (argc < 3) {
            std::cerr << "Usage: axon artifact-retrieve <artifact_id>\n";
            return 1;
        }
        std::string artifact_id = argv[2];
        auto cfg = load_config();

        std::optional<axon::CcrArtifact> artifact;
        bool db_locked = false;
        if (fs::exists(cfg.db_path)) {
            try {
                axon::Database db(cfg.db_path);
                artifact = axon::ccr_retrieve_artifact(db, artifact_id);
            } catch (const std::exception&) {
                // Typically another axon process holds the write lock; the
                // sidecar and peer fallbacks below still allow recovery.
                db_locked = true;
            }
        }

        if (!artifact)
            artifact = axon::ccr_retrieve_artifact_file(cfg.axon_dir / "ccr", artifact_id);

        if (!artifact && db_locked) {
            // The lock holder can read the DB row for us (#58 peer protocol).
            axon::mcp::ServerContext ctx;
            ctx.cfg = cfg;
            std::string proxy_error;
            auto proxied = axon::mcp::proxy_tool_call(ctx, "artifact_retrieve",
                                                      {{"artifact_id", artifact_id}}, proxy_error);
            if (proxied) {
                try {
                    auto content = proxied->value("content", nlohmann::json::array());
                    if (content.is_array() && !content.empty()) {
                        auto payload =
                            nlohmann::json::parse(content[0].value("text", "{}"), nullptr, false);
                        if (payload.is_object() && !payload.contains("error")) {
                            axon::CcrArtifact a;
                            a.artifact_id = payload.value("artifact_id", artifact_id);
                            a.kind = payload.value("kind", "");
                            a.source_ref = payload.value("source_ref", "");
                            a.content = payload.value("content", "");
                            a.token_estimate =
                                payload.value("token_estimate", static_cast<int64_t>(0));
                            artifact = std::move(a);
                        }
                    }
                } catch (const std::exception&) {
                }
            }
        }

        if (!artifact) {
            if (!fs::exists(cfg.db_path)) {
                std::cerr << "No index found. Run `axon index` first.\n";
            } else if (db_locked) {
                std::cerr << "Artifact not found: " << artifact_id
                          << " (index locked by another axon process; sidecar and"
                             " peer lookups also missed)\n";
            } else {
                std::cerr << "Artifact not found: " << artifact_id << "\n";
            }
            return 1;
        }
        std::cout << artifact->content;
        return 0;
    }

    // ── axon filter <kind> [--budget=N] ───────────────────────────────────
    if (cmd == "filter") {
        std::string kind = argc > 2 ? argv[2] : "auto";
        int budget = 800;
        bool json_metrics = false;
        for (int i = 3; i < argc; i++) {
            std::string a = argv[i];
            if (a.rfind("--budget=", 0) == 0) {
                try {
                    budget = std::stoi(a.substr(9));
                } catch (...) {
                    budget = 800;
                }
            } else if (a == "--metrics=json" || a == "--json-metrics") {
                json_metrics = true;
            }
        }

        std::ostringstream ss;
        ss << std::cin.rdbuf();
        std::string input = ss.str();

        auto start = std::chrono::steady_clock::now();
        auto filtered = axon::filter_shell_output(kind, input, budget);
        std::string ccr_artifact_id;
        if (filtered.changed) {
            auto cfg = load_config();
            // Quiet open: when another process (axon serve/web) holds the
            // DuckDB lock, fall back to the file sidecar store instead of
            // passing input through unfiltered — hooks run filter on every
            // command, so the lock is the common case, not the exception.
            std::unique_ptr<axon::Database> db;
            try {
                db = std::make_unique<axon::Database>(cfg.db_path);
            } catch (const std::exception&) {
                db = nullptr;
            }
            axon::CcrStoreFn store;
            if (db) {
                store = [&db](const std::string& k, const std::string& ref,
                              const std::string& content, int64_t tokens) {
                    return axon::ccr_store_artifact(*db, k, ref, content, tokens);
                };
            } else {
                auto ccr_dir = cfg.axon_dir / "ccr";
                store = [ccr_dir](const std::string& k, const std::string& ref,
                                  const std::string& content, int64_t tokens) {
                    return axon::ccr_store_artifact_file(ccr_dir, k, ref, content, tokens);
                };
            }
            auto make_recoverable = [&](const axon::ShellFilterResult& candidate) {
                return axon::ccr_make_recoverable_output(
                    store, "shell_filter", "filter." + candidate.command + ":stdin", input,
                    candidate.output, candidate.input_tokens);
            };

            auto recoverable = make_recoverable(filtered);
            if (recoverable.recoverable && recoverable.output_tokens > budget &&
                !recoverable.artifact_id.empty()) {
                int marker_tokens = estimate_tokens_cli(
                    axon::ccr_marker(recoverable.artifact_id, filtered.input_tokens));
                int adjusted_budget = budget - marker_tokens;
                if (adjusted_budget > 0 && adjusted_budget < budget) {
                    auto tighter = axon::filter_shell_output(kind, input, adjusted_budget);
                    if (tighter.changed) {
                        auto tighter_recoverable = make_recoverable(tighter);
                        if (tighter_recoverable.recoverable)
                            recoverable = std::move(tighter_recoverable);
                    }
                }
            }

            if (recoverable.recoverable && recoverable.output_tokens <= budget) {
                filtered.output = std::move(recoverable.output);
                filtered.output_tokens = static_cast<int>(recoverable.output_tokens);
                filtered.tokens_saved = static_cast<int>(recoverable.tokens_saved);
                ccr_artifact_id = std::move(recoverable.artifact_id);
            } else {
                filtered.output = input;
                filtered.output_tokens = filtered.input_tokens;
                filtered.tokens_saved = 0;
                filtered.changed = false;
            }
        }
        std::cout << filtered.output;
        int64_t filter_latency_ms = elapsed_ms(start);
        if (json_metrics) {
            nlohmann::json metrics = {{"type", "axon_filter_metrics"},
                                      {"command", filtered.command},
                                      {"kind", axon::output_kind_to_string(filtered.kind)},
                                      {"budget", budget},
                                      {"input_tokens", filtered.input_tokens},
                                      {"output_tokens", filtered.output_tokens},
                                      {"tokens_saved", filtered.tokens_saved},
                                      {"changed", filtered.changed},
                                      {"layer", "shell_filtering"},
                                      {"latency_ms", filter_latency_ms}};
            if (!ccr_artifact_id.empty()) {
                metrics["ccr_artifact_id"] = ccr_artifact_id;
                metrics["recoverable"] = true;
            } else {
                metrics["recoverable"] = false;
            }
            std::cerr << metrics.dump() << "\n";
        } else {
            std::cerr << "[axon filter] kind=" << axon::output_kind_to_string(filtered.kind)
                      << " input_tokens=" << filtered.input_tokens
                      << " output_tokens=" << filtered.output_tokens
                      << " saved=" << filtered.tokens_saved
                      << " changed=" << (filtered.changed ? "true" : "false");
            if (!ccr_artifact_id.empty()) std::cerr << " ccr_artifact_id=" << ccr_artifact_id;
            std::cerr << "\n";
        }

        if (const char* telemetry_env = std::getenv("AXON_TELEMETRY")) {
            std::string enabled = telemetry_env;
            if (enabled == "1" || enabled == "true" || enabled == "yes" || enabled == "on") {
                auto cfg = load_config();
                if (fs::exists(cfg.db_path)) {
                    auto db = open_database_or_report(cfg.db_path);
                    if (db) {
                        axon::record_telemetry(cfg, db.get(),
                                               {"filter." + filtered.command, "shell",
                                                filter_latency_ms, filtered.output_tokens,
                                                filtered.input_tokens, filtered.tokens_saved, false,
                                                "shell_filtering"});
                        if (!ccr_artifact_id.empty()) {
                            axon::record_telemetry(
                                cfg, db.get(),
                                {"filter.ccr_store", "shell", 0, 0, 0, 0, false, "ccr"});
                        }
                    }
                }
            }
        }
        return 0;
    }

    // ── axon skeleton <file> ───────────────────────────────────────────────
    if (cmd == "skeleton") {
        if (argc < 3) {
            std::cerr << "Usage: axon skeleton <file>\n";
            return 1;
        }
        fs::path p = argv[2];
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            std::cerr << "File not found: " << p << "\n";
            return 1;
        }
        std::string src((std::istreambuf_iterator<char>(f)), {});

        auto ext = p.extension().string();
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        auto lang = axon::language_from_extension(ext);
        if (!lang) {
            std::cerr << "Unsupported language: " << ext << "\n";
            return 1;
        }

        std::cout << axon::skeletonize(src, *lang);
        return 0;
    }

    // ── axon status ────────────────────────────────────────────────────────
    if (cmd == "status") {
        std::string path = argc > 2 ? argv[2] : "";
        if (!path.empty() && !fs::exists(path)) {
            std::cerr << "Error: path does not exist: " << path << "\n";
            return 1;
        }
        auto cfg = load_config(path);
        if (!fs::exists(cfg.db_path)) {
            std::cout << "No index. Run `axon index`.\n";
            return 0;
        }

        auto db = open_database_or_report(cfg.db_path);
        if (!db) return 1;
        auto fr = db->conn().Query("SELECT count(*) FROM files");
        auto sr = db->conn().Query("SELECT count(*) FROM symbols");
        auto er = db->conn().Query("SELECT count(*) FROM edges");
        auto embr = db->conn().Query("SELECT count(*) FROM symbols WHERE embedding IS NOT NULL");
        auto& fm = *fr;
        auto& sm = *sr;
        auto& em = *er;
        auto& embm = *embr;

        std::cout << "Project: " << cfg.project_root << "\n";
        std::cout << "Files:    " << fm.GetValue<int64_t>(0, 0) << "\n";
        std::cout << "Symbols:  " << sm.GetValue<int64_t>(0, 0) << "\n";
        std::cout << "Edges:    " << em.GetValue<int64_t>(0, 0) << "\n";
        std::cout << "Embedded: " << embm.GetValue<int64_t>(0, 0) << " symbols\n";
        return 0;
    }

    // ── axon portfolio / capability ───────────────────────────────────────
    // These commands are deliberately backed by the derived local catalog, rather than by
    // ad-hoc scans, so all delivery surfaces observe the same bounded projection.
    if (cmd == "portfolio" || cmd == "capability") {
        if (argc < 3) { std::cerr << "portfolio/capability subcommand required\n"; return 1; }
        const std::string sub = argv[2];
        std::optional<std::string> group, repo;
        double threshold = 0.0;
        std::vector<std::string> positional;
        for (int i = 3; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--json") continue; // portfolio output is intentionally JSON-stable
            else if (argument.rfind("--group=", 0) == 0) group = argument.substr(8);
            else if (argument.rfind("--repo=", 0) == 0) repo = argument.substr(7);
            else if (argument.rfind("--threshold=", 0) == 0) { try { threshold = std::stod(argument.substr(12)); } catch (...) { std::cerr << "invalid threshold\n"; return 1; } }
            else positional.push_back(argument);
        }
        try {
            axon::portfolio::PortfolioCapabilityCatalog catalog;
            auto signature_json = [](const axon::portfolio::CapabilitySignature& signature) {
                return nlohmann::json{{"id", signature.signature_id}, {"repository_id", signature.stream.repository_id},
                    {"index_stream_id", signature.stream.index_stream_id}, {"name", signature.normalized_name},
                    {"path", signature.path.value_or("")}, {"epoch", signature.index_epoch},
                    {"summary", signature.deterministic_summary}, {"routes", signature.routes},
                    {"contracts", signature.contracts}};
            };
            if (cmd == "portfolio") {
                if (sub == "sync" || sub == "reconcile" || sub == "rebuild") {
                    const auto report = catalog.sync(group, sub == "rebuild"); nlohmann::json items = nlohmann::json::array();
                    for (const auto& item : report.repositories) items.push_back({{"repository_id",item.repository_id},{"index_stream_id",item.index_stream_id},{"status",item.status},{"detail",item.detail},{"signatures",item.signatures}});
                    std::cout << nlohmann::json{{"degraded",report.degraded},{"repositories",items}}.dump(2) << "\n"; return report.degraded ? 2 : 0;
                }
                if (sub == "status") { const auto status = catalog.status(); nlohmann::json repos=nlohmann::json::array(); for(const auto& item:status.repositories) repos.push_back({{"repository_id",item.repository_id},{"index_stream_id",item.index_stream_id},{"status",item.status},{"detail",item.detail}}); std::cout << nlohmann::json{{"catalog_path",catalog.path().string()},{"capabilities",catalog.list({},10000).size()},{"degraded",status.degraded},{"repositories",repos}}.dump(2) << "\n"; return status.degraded?2:0; }
            } else if (sub == "list") { nlohmann::json output=nlohmann::json::array(); for(const auto& signature:catalog.list(repo,200)) output.push_back(signature_json(signature)); std::cout<<output.dump(2)<<"\n"; return 0;
            } else if (sub == "search" && positional.size()==1) { nlohmann::json output=nlohmann::json::array(); for(const auto& signature:catalog.search(positional[0])) output.push_back(signature_json(signature)); std::cout<<output.dump(2)<<"\n"; return 0;
            } else if (sub == "duplicates") { nlohmann::json output=nlohmann::json::array(); for(const auto& candidate:catalog.duplicates(threshold)) output.push_back({{"id",candidate.candidate_id},{"left",candidate.left_capability_id},{"right",candidate.right_capability_id},{"score",candidate.final_score},{"classification",axon::portfolio::to_string(candidate.classification)},{"recommendation",axon::portfolio::to_string(candidate.recommendation)},{"differences",candidate.differences},{"invalidators",candidate.invalidators}}); std::cout<<output.dump(2)<<"\n"; return 0;
            } else if (sub == "compare" && positional.size()==1) { for(const auto& candidate:catalog.duplicates(0.0,10000)) if(candidate.candidate_id==positional[0]) { std::cout<<nlohmann::json{{"id",candidate.candidate_id},{"score",candidate.final_score},{"classification",axon::portfolio::to_string(candidate.classification)},{"signals",candidate.signals.size()},{"differences",candidate.differences},{"invalidators",candidate.invalidators}}.dump(2)<<"\n"; return 0; } std::cerr<<"candidate not found\n"; return 1;
            } else if (sub == "drift" && positional.size()==2) { const auto drift=catalog.drift(positional[0],positional[1]); nlohmann::json output={{"matches",drift.matches.size()},{"drift",drift.drift.size()}}; std::cout<<output.dump(2)<<"\n"; return 0; }
            std::cerr << "invalid portfolio/capability command arguments\n"; return 1;
        } catch (const std::exception& error) { std::cerr << "portfolio: " << error.what() << "\n"; return 1; }
    }

    // ── axon metrics [--json] ──────────────────────────────────────────────
    // The per-layer telemetry aggregate that `serve --http` exposes at
    // /api/metrics, but from the CLI so it has a consumer without standing up
    // the HTTP server. Human table by default; --json prints the raw object.
    if (cmd == "metrics") {
        bool as_json = false;
        for (int i = 2; i < argc; i++)
            if (std::string(argv[i]) == "--json") as_json = true;

        auto cfg = load_config();
        std::unique_ptr<axon::Database> db;
        if (fs::exists(cfg.db_path)) {
            db = open_database_or_report(cfg.db_path);
            if (!db) return 1;
        }
        auto metrics = axon::metrics_json(cfg, db.get());

        if (as_json) {
            std::cout << metrics.dump(2) << "\n";
            return 0;
        }

        if (!metrics.value("telemetry_enabled", false)) {
            std::cout << "Telemetry is disabled (opt-in). Enable with "
                         "`telemetry = true` in .axon/config.toml or AXON_TELEMETRY=1.\n";
            return 0;
        }
        std::cout << "Telemetry (per layer):\n";
        printf("  %-16s %8s %10s %12s %8s\n", "layer", "requests", "tokens_out", "tokens_saved",
               "avg_ms");
        for (const auto& layer : {"retrieval", "cache", "ccr", "compression", "shell_filtering"}) {
            auto it = metrics.find("layers");
            if (it == metrics.end() || !it->contains(layer)) continue;
            const auto& L = (*it)[layer];
            printf("  %-16s %8lld %10lld %12lld %8.1f\n", layer, (long long)L.value("requests", 0),
                   (long long)L.value("tokens_sent", 0), (long long)L.value("tokens_saved", 0),
                   L.value("average_latency_ms", 0.0));
        }
        return 0;
    }

    // ── axon doctor locks [--json] ─────────────────────────────────────────
    if (cmd == "doctor") {
        std::string sub = argc > 2 ? argv[2] : "";
        bool json_output = argc > 3 && std::string(argv[3]) == "--json";
        if (sub != "locks" || (argc > 3 && !json_output) || argc > 4) {
            std::cerr << "Usage: axon doctor locks [--json]\n";
            return 1;
        }

        nlohmann::json reports = nlohmann::json::array();
        int unhealthy = 0;
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        for (const auto& entry : axon::load_registry().repos) {
            if (entry.owner_pid == 0) continue;
            nlohmann::json report = {
                {"root", entry.root},
                {"pid", entry.owner_pid},
                {"port", entry.owner_port},
                {"process_alive", axon::mcp::pid_alive(entry.owner_pid)},
                {"heartbeat_age_seconds", entry.owner_heartbeat_at > 0
                                              ? std::max(0LL, now - entry.owner_heartbeat_at)
                                              : -1LL}};
            std::string status;
            std::string detail;
            if (!report["process_alive"].get<bool>()) {
                status = "dead";
                detail = "registered owner process is not running";
            } else if (entry.owner_port <= 0) {
                status = "unreachable";
                detail = "registered owner has no peer port";
            } else {
                std::string probe_error;
                auto health = axon::mcp::probe_peer_health(entry.owner_port, probe_error);
                if (!health) {
                    status = "unresponsive";
                    detail = probe_error;
                } else if (health->value("pid", 0LL) != entry.owner_pid ||
                           health->value("root", "") != entry.root) {
                    status = "identity_mismatch";
                    detail = "peer identity does not match registry owner";
                } else if (!health->value("db_ready", false)) {
                    status = "released";
                    detail = "peer is responsive but no longer holds the database";
                } else {
                    status = "healthy";
                    if (health->contains("idle_seconds"))
                        report["idle_seconds"] = (*health)["idle_seconds"];
                }
            }
            report["status"] = status;
            if (!detail.empty()) report["detail"] = detail;
            if (status != "healthy") unhealthy++;
            reports.push_back(std::move(report));
        }

        if (json_output) {
            std::cout << nlohmann::json{{"owners", reports},
                                        {"owner_count", reports.size()},
                                        {"unhealthy_count", unhealthy}}
                             .dump(2)
                      << "\n";
        } else if (reports.empty()) {
            std::cout << "No registered DuckDB lock owners.\n";
        } else {
            for (const auto& report : reports) {
                std::cout << report["status"].get<std::string>() << " pid=" << report["pid"]
                          << " root=" << report["root"].get<std::string>();
                if (report.contains("idle_seconds"))
                    std::cout << " idle_seconds=" << report["idle_seconds"];
                if (report.contains("detail"))
                    std::cout << " detail=" << report["detail"].get<std::string>();
                std::cout << "\n";
            }
        }
        return unhealthy == 0 ? 0 : 2;
    }

    // ── axon registry prune ────────────────────────────────────────────────
    if (cmd == "registry") {
        std::string sub = argc > 2 ? argv[2] : "";
        if (sub != "prune") {
            std::cerr << "Usage: axon registry prune\n";
            return 1;
        }
        auto before = axon::load_registry().repos.size();
        int removed = axon::prune_registry();
        std::cout << "Pruned " << removed << " dead " << (removed == 1 ? "entry" : "entries")
                  << " (kept " << (before - removed) << ") in " << axon::registry_path().string()
                  << "\n";
        return 0;
    }

    print_usage();
    return 1;
}
