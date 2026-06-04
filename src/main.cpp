#include "core/config.hpp"
#include "core/registry.hpp"
#include "core/db.hpp"
#include "core/indexer.hpp"
#include "core/graph.hpp"
#include "core/skeleton.hpp"
#include "core/capsule.hpp"
#include "core/embeddings.hpp"
#include "core/telemetry.hpp"
#include "mcp/server.hpp"
#include "mcp/http_server.hpp"
#include "lsp/server.hpp"
#include "version.hpp"
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

static std::atomic<bool> g_watch_running{true};

static void stop_watch(int) {
    g_watch_running = false;
}

static int64_t elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
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
  axon watch [path] [--interval-ms=1000] [--debounce-ms=500]
                                        Poll for external edits and incrementally reindex
  axon capsule <query> [--no-cache]     Print context capsule for a query
  axon skeleton <file>                  Print skeleton (signatures-only) of a file
  axon status                           Show index statistics
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

static axon::mcp::ServerContext make_server_context(const char* binary_path, bool load_model = true) {
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
                } catch (...) {}
            }
        } catch (const std::exception& e) {
            ctx.db_error = e.what();
        }
    }

    return ctx;
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return 1; }
    std::string cmd = argv[1];

    // ── axon --version | -V | version ─────────────────────────────────────
    if (cmd == "--version" || cmd == "-V" || cmd == "version") {
        std::cout << "axon " << axon::VERSION
                  << " (build " << axon::GIT_SHA << ")\n";
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
            if (a == "--force" || a == "-f") force = true;
            else if (path.empty()) path = a;
        }
        if (!path.empty() && !fs::exists(path)) {
            std::cerr << "Error: path does not exist: " << path << "\n";
            return 1;
        }
        auto cfg = load_config(path);
        std::cout << "Indexing " << cfg.project_root
                  << (force ? " (force)" : "") << " ...\n";

        auto db = open_database_or_report(cfg.db_path);
        if (!db) return 1;
        auto start = std::chrono::steady_clock::now();

        auto stats = axon::index_project(cfg, *db, [](const std::string& f, int done, int total) {
            std::cerr << "\r[" << done << "/" << total << "] " << f << "    ";
        }, force);
        std::cerr << "\n";

        std::cout << "Done: " << stats.files_indexed << " files, "
                  << stats.symbols_found << " symbols, "
                  << stats.edges_found   << " edges\n";

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
        axon::record_telemetry(cfg, db.get(), {
            "index", "cli", elapsed_ms(start),
            stats.symbols_found * 12, stats.files_indexed * 500, 0, false
        });
        return 0;
    }

    // ── axon index-paths <files...> [--prune] ──────────────────────────────
    if (cmd == "index-paths") {
        bool prune = false;
        std::vector<fs::path> paths;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--prune") prune = true;
            else paths.push_back(fs::path(a));
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

        std::cout << stats.files_indexed << " indexed, "
                  << stats.files_skipped << " unchanged";
        if (prune) std::cout << ", " << stats.files_pruned << " pruned";
        std::cout << "\n";
        axon::record_telemetry(cfg, db.get(), {
            "index-paths", "cli", elapsed_ms(start),
            stats.symbols_found * 12, stats.files_indexed * 500, 0, false
        });
        return 0;
    }

    // ── axon watch [path] [--interval-ms=N] [--debounce-ms=N] ─────────────
    if (cmd == "watch") {
        std::string path;
        int interval_ms = 1000;
        int debounce_ms = 500;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a.rfind("--interval-ms=", 0) == 0) interval_ms = std::stoi(a.substr(14));
            else if (a.rfind("--debounce-ms=", 0) == 0) debounce_ms = std::stoi(a.substr(14));
            else if (path.empty()) path = a;
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

        struct Stamp { int64_t mtime = 0; uintmax_t size = 0; };
        auto stamp = [](const fs::path& p) {
            std::error_code ec;
            auto t = fs::last_write_time(p, ec);
            auto s = fs::file_size(p, ec);
            return Stamp{
                ec ? int64_t{0} : static_cast<int64_t>(t.time_since_epoch().count()),
                ec ? uintmax_t{0} : s
            };
        };
        auto snapshot = [&]() {
            std::unordered_map<std::string, Stamp> out;
            for (auto it = fs::recursive_directory_iterator(
                     cfg.project_root, fs::directory_options::skip_permission_denied);
                 it != fs::end(it); ++it) {
                if (!it->is_regular_file()) continue;
                auto ext = it->path().extension().string();
                if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
                if (!axon::language_from_extension(ext)) continue;
                std::error_code ec;
                auto rel = fs::relative(it->path(), cfg.project_root, ec);
                if (!ec && !rel.empty() && rel.generic_string().rfind("..", 0) != 0)
                    out[rel.generic_string()] = stamp(it->path());
            }
            return out;
        };

        auto seen = snapshot();
        std::cout << "Watching " << cfg.project_root << " (poll "
                  << interval_ms << "ms, debounce " << debounce_ms << "ms)\n";
        std::cout.flush();

        while (g_watch_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            auto next = snapshot();
            std::vector<fs::path> changed;
            bool deleted = false;

            for (const auto& [path_key, st] : next) {
                auto it = seen.find(path_key);
                if (it == seen.end() || it->second.mtime != st.mtime || it->second.size != st.size)
                    changed.emplace_back(path_key);
            }
            for (const auto& [path_key, st] : seen) {
                if (!next.count(path_key)) deleted = true;
            }
            if (changed.empty() && !deleted) {
                seen = std::move(next);
                continue;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(debounce_ms));
            auto start = std::chrono::steady_clock::now();
            auto stats = axon::index_files(cfg, *db, changed, deleted);
            std::cout << stats.files_indexed << " indexed, "
                      << stats.files_skipped << " unchanged";
            if (deleted) std::cout << ", " << stats.files_pruned << " pruned";
            std::cout << "\n";
            std::cout.flush();
            axon::record_telemetry(cfg, db.get(), {
                "watch", "cli", elapsed_ms(start),
                stats.symbols_found * 12, stats.files_indexed * 500, 0, false
            });
            seen = snapshot();
        }
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
            if (a == "--http") use_http = true;
            else if (a.rfind("--port=", 0) == 0) http_port = std::stoi(a.substr(7));
            else if (a.rfind("--host=", 0) == 0) http_host = a.substr(7);
            else if (a.rfind("--group=", 0) == 0) http_group = a.substr(8);
            else if (a == "--all") http_all_repos = true;
        }

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
        if (argc < 3) { std::cerr << "Usage: axon capsule <query> [--no-cache]\n"; return 1; }
        std::string query;
        bool no_cache = false;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--no-cache") { no_cache = true; continue; }
            if (!query.empty()) query += " ";
            query += a;
        }

        auto cfg = load_config();
        if (!fs::exists(cfg.db_path)) { std::cerr << "No index found. Run `axon index` first.\n"; return 1; }

        auto db = open_database_or_report(cfg.db_path);
        if (!db) return 1;
        auto graph = axon::load_graph(*db);
        auto start = std::chrono::steady_clock::now();

        // Cache check (W2.T01) — skipped under --no-cache so devs can force
        // a fresh assemble after parser/grammar changes that would otherwise
        // be served from a stale entry.
        const std::string epoch = axon::current_project_epoch(*db);
        const std::string cache_key = axon::compute_capsule_cache_key(
            query, cfg.project_cfg.token_budget, epoch);
        if (!no_cache) {
            if (auto hit = axon::capsule_cache_lookup(*db, cache_key, epoch)) {
                std::cout << "{\n";
                std::cout << "  \"query\": \"" << hit->query << "\",\n";
                std::cout << "  \"token_estimate\": " << hit->token_estimate << ",\n";
                std::cout << "  \"pivot_files\": " << hit->pivot_files.size() << ",\n";
                std::cout << "  \"support_files\": " << hit->support_files.size() << ",\n";
                std::cout << "  \"cache\": \"hit\"\n";
                std::cout << "}\n";
                for (const auto& f : hit->pivot_files)
                    std::cerr << "  [pivot]   " << f.path << " (" << f.token_estimate << " tok)\n";
                for (const auto& f : hit->support_files)
                    std::cerr << "  [support] " << f.path << " (" << f.token_estimate << " tok)\n";
                axon::record_telemetry(cfg, db.get(), {
                    "capsule", "cli", elapsed_ms(start),
                    hit->token_estimate, hit->token_estimate * 4, hit->token_estimate * 3, true
                });
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

        auto capsule = axon::assemble_capsule(query, {}, *db, model, graph, cfg.project_root,
                                              cfg.project_cfg.token_budget);
        if (!no_cache) {
            axon::capsule_cache_insert(*db, cache_key, epoch, capsule);
        }

        std::cout << "{\n";
        std::cout << "  \"query\": \"" << query << "\",\n";
        std::cout << "  \"token_estimate\": " << capsule.token_estimate << ",\n";
        std::cout << "  \"pivot_files\": " << capsule.pivot_files.size() << ",\n";
        std::cout << "  \"support_files\": " << capsule.support_files.size() << "\n";
        std::cout << "}\n";

        for (const auto& f : capsule.pivot_files)
            std::cerr << "  [pivot]   " << f.path << " (" << f.token_estimate << " tok)\n";
        for (const auto& f : capsule.support_files)
            std::cerr << "  [support] " << f.path << " (" << f.token_estimate << " tok)\n";
        axon::record_telemetry(cfg, db.get(), {
            "capsule", "cli", elapsed_ms(start),
            capsule.token_estimate, capsule.token_estimate * 4, capsule.token_estimate * 3, false
        });
        return 0;
    }

    // ── axon skeleton <file> ───────────────────────────────────────────────
    if (cmd == "skeleton") {
        if (argc < 3) { std::cerr << "Usage: axon skeleton <file>\n"; return 1; }
        fs::path p = argv[2];
        std::ifstream f(p, std::ios::binary);
        if (!f) { std::cerr << "File not found: " << p << "\n"; return 1; }
        std::string src((std::istreambuf_iterator<char>(f)), {});

        auto ext = p.extension().string();
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        auto lang = axon::language_from_extension(ext);
        if (!lang) { std::cerr << "Unsupported language: " << ext << "\n"; return 1; }

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
        if (!fs::exists(cfg.db_path)) { std::cout << "No index. Run `axon index`.\n"; return 0; }

        auto db = open_database_or_report(cfg.db_path);
        if (!db) return 1;
        auto fr  = db->conn().Query("SELECT count(*) FROM files");
        auto sr  = db->conn().Query("SELECT count(*) FROM symbols");
        auto er  = db->conn().Query("SELECT count(*) FROM edges");
        auto embr= db->conn().Query("SELECT count(*) FROM symbols WHERE embedding IS NOT NULL");
        auto& fm  = *fr;
        auto& sm  = *sr;
        auto& em  = *er;
        auto& embm= *embr;

        std::cout << "Project: " << cfg.project_root << "\n";
        std::cout << "Files:    " << fm.GetValue<int64_t>(0, 0) << "\n";
        std::cout << "Symbols:  " << sm.GetValue<int64_t>(0, 0) << "\n";
        std::cout << "Edges:    " << em.GetValue<int64_t>(0, 0) << "\n";
        std::cout << "Embedded: " << embm.GetValue<int64_t>(0, 0) << " symbols\n";
        return 0;
    }

    print_usage();
    return 1;
}
