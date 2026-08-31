#include "routes.hpp"
#include "portfolio/domain/index_journal.hpp"
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
#include <iostream>
#include <unordered_set>

namespace axon {
namespace fs = std::filesystem;

static const std::vector<std::string> SKIP_DIRS = {"node_modules", ".git",  "target", "build",
                                                   "__pycache__",  ".axon", "dist",   ".next",
                                                   "vendor",       ".venv", "venv",   ".worktrees"};

static bool should_skip(const fs::path& p) {
    for (const auto& d : SKIP_DIRS)
        if (p.filename() == d) return true;
    return false;
}

static std::string sq(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\'') o += '\'';
        o += c;
    }
    return o;
}

// Convert a Next.js file path to route URL
// app/users/[id]/route.ts → /users/[id]
// pages/api/auth.ts       → /api/auth
static std::string nextjs_file_to_route(const std::string& rel_path) {
    std::string p = rel_path;
    // Strip leading "app/" or "pages/"
    if (p.rfind("app/", 0) == 0)
        p = p.substr(4);
    else if (p.rfind("pages/", 0) == 0)
        p = p.substr(6);
    // Strip filename (route.ts, page.tsx, index.ts, etc.)
    auto slash = p.find_last_of('/');
    if (slash != std::string::npos)
        p = p.substr(0, slash);
    else
        p = "";
    // Strip extension from p if no slash (root level file)
    auto dot = p.find_last_of('.');
    if (dot != std::string::npos && p.find('/') == std::string::npos) p = p.substr(0, dot);
    return "/" + p;
}

static void detect_nextjs_routes(const fs::path& root, const std::string& rel_path,
                                 std::vector<RouteInfo>& out) {
    // Next.js App Router: app/**/route.{ts,js,tsx,jsx}
    std::string fname = fs::path(rel_path).filename().string();
    if (fname == "route.ts" || fname == "route.js" || fname == "route.tsx" ||
        fname == "route.jsx") {
        if (rel_path.find("app/") == 0 || rel_path.find("/app/") != std::string::npos) {
            RouteInfo r;
            r.method = "ANY";
            r.path = nextjs_file_to_route(rel_path);
            r.handler_file = rel_path;
            r.framework = "nextjs";
            out.push_back(r);
            return;
        }
    }
    // Next.js Pages Router: pages/api/**
    if (rel_path.find("pages/api/") == 0 || rel_path.find("/pages/api/") != std::string::npos) {
        RouteInfo r;
        r.method = "ANY";
        r.path = nextjs_file_to_route(rel_path);
        r.handler_file = rel_path;
        r.framework = "nextjs";
        out.push_back(r);
    }
}

static void detect_express_routes(const std::string& content, const std::string& rel_path,
                                  std::vector<RouteInfo>& out) {
    // Match: app.get('/path', ...) or router.post('/path', ...) etc.
    std::regex re(R"((app|router)\.(get|post|put|delete|patch|use)\s*\(\s*['"`]([^'"`]+)['"`])",
                  std::regex::ECMAScript | std::regex::icase);
    auto begin = std::sregex_iterator(content.begin(), content.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        RouteInfo r;
        r.method = (*it)[2].str();
        std::string m = r.method;
        for (auto& c : m)
            c = toupper(c);
        r.method = m;
        r.path = (*it)[3].str();
        r.handler_file = rel_path;
        r.framework = "express";
        out.push_back(r);
    }
}

static void detect_python_routes(const std::string& content, const std::string& rel_path,
                                 std::vector<RouteInfo>& out) {
    // FastAPI/Flask: @router.get("/path") @app.route("/path", methods=[...])
    std::regex fastapi_re(R"(@(?:router|app)\.(get|post|put|delete|patch)\s*\(\s*['"]([^'"]+)['"])",
                          std::regex::ECMAScript | std::regex::icase);
    std::regex flask_re(
        R"(@(?:app|bp|blueprint)\s*\.\s*route\s*\(\s*['"]([^'"]+)['"](?:[^)]*methods\s*=\s*\[([^\]]*)\])?)",
        std::regex::ECMAScript | std::regex::icase);

    auto begin = std::sregex_iterator(content.begin(), content.end(), fastapi_re);
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        RouteInfo r;
        r.method = (*it)[1].str();
        for (auto& c : r.method)
            c = toupper(c);
        r.path = (*it)[2].str();
        r.handler_file = rel_path;
        r.framework = "fastapi";
        out.push_back(r);
    }

    begin = std::sregex_iterator(content.begin(), content.end(), flask_re);
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        RouteInfo r;
        r.method = "ANY";
        r.path = (*it)[1].str();
        r.handler_file = rel_path;
        r.framework = "flask";
        out.push_back(r);
    }
}

int index_routes(const Config& cfg, Database& db) {
    // Ensure routes table exists
    db.exec("CREATE TABLE IF NOT EXISTS routes ("
            "  id           BIGINT PRIMARY KEY,"
            "  method       VARCHAR NOT NULL,"
            "  path         VARCHAR NOT NULL,"
            "  handler_file VARCHAR NOT NULL,"
            "  framework    VARCHAR NOT NULL DEFAULT 'unknown',"
            "  file_id      BIGINT"
            ")");
    std::vector<RouteInfo> found;

    for (auto it = fs::recursive_directory_iterator(cfg.project_root,
                                                    fs::directory_options::skip_permission_denied);
         it != fs::end(it); ++it) {
        if (it->is_directory() && should_skip(it->path())) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;

        auto abs = it->path();
        auto rel = fs::relative(abs, cfg.project_root).generic_string();
        auto ext = abs.extension().string();
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);

        bool is_ts_js =
            (ext == "ts" || ext == "tsx" || ext == "js" || ext == "jsx" || ext == "mjs");
        bool is_py = (ext == "py");
        if (!is_ts_js && !is_py) continue;

        // Next.js detection (file path based — no content read needed)
        detect_nextjs_routes(cfg.project_root, rel, found);

        // Content-based detection
        std::ifstream f(abs, std::ios::binary);
        if (!f) continue;
        std::string content((std::istreambuf_iterator<char>(f)), {});

        if (is_ts_js) detect_express_routes(content, rel, found);
        if (is_py) detect_python_routes(content, rel, found);
    }

    // Deduplicate and insert
    auto& conn = db.conn();
    portfolio::Transaction transaction(conn);
    auto previous = conn.Query("SELECT method,path,handler_file FROM routes");
    require_ok(previous, "load existing routes");
    std::unordered_set<std::string> previous_keys;
    for (duckdb::idx_t row = 0; row < previous->RowCount(); ++row)
        previous_keys.insert(previous->GetValue(0, row).ToString() + " " +
                             previous->GetValue(1, row).ToString() + "@" +
                             previous->GetValue(2, row).ToString());
    require_success(conn.Query("DELETE FROM routes"), "replace routes");
    if (!previous_keys.empty() || !found.empty()) transaction.mark_index_mutation();
    int inserted = 0;
    std::vector<portfolio::AffectedEntity> affected;
    std::unordered_set<std::string> current_keys;
    for (const auto& r : found) {
        // Resolve file_id
        std::string fid_sql = "SELECT id FROM files WHERE path = '" + sq(r.handler_file) + "'";
        auto fid_res = conn.Query(fid_sql);
        require_ok(fid_res, "resolve route handler file");
        std::string fid_str = "NULL";
        if (fid_res->RowCount() > 0)
            fid_str = std::to_string(fid_res->GetValue<int64_t>(0, 0));

        require_success(conn.Query("INSERT INTO routes (id, method, path, handler_file, framework, "
                                   "file_id) VALUES (nextval('seq_id'), '" +
                                   sq(r.method) + "', '" + sq(r.path) + "', '" +
                                   sq(r.handler_file) + "', '" + sq(r.framework) + "', " +
                                   fid_str + ")"),
                        "insert route");
        const std::string route_key = r.method + " " + r.path + "@" + r.handler_file;
        current_keys.insert(route_key);
        affected.push_back({"route", route_key, "upsert", std::nullopt});
        inserted++;
    }
    std::vector<portfolio::AffectedEntity> deleted;
    for (const auto& route_key : previous_keys)
        if (current_keys.count(route_key) == 0) {
            deleted.push_back({"route", route_key, "delete", std::nullopt});
            affected.push_back(deleted.back());
        }
    if (!affected.empty()) {
        for (const auto& route_key : current_keys)
            portfolio::clear_tombstone(
                conn, {"route", route_key, "upsert", std::nullopt});
        portfolio::trigger_journal_failpoint_for_testing("after_mutation");
        const std::string manifest = portfolio::compute_manifest_hash(conn);
        const uint64_t sequence =
            portfolio::append_index_event(transaction, conn, "IndexRoutesUpdated", affected,
                                          manifest);
        const std::string epoch = portfolio::index_identity(conn).current_epoch;
        for (const auto& route : deleted)
            portfolio::upsert_tombstone(conn, route, sequence, epoch);
    }
    transaction.commit();
    return inserted;
}

std::vector<RouteInfo> get_all_routes(Database& db) {
    std::vector<RouteInfo> routes;
    try {
        auto res = db.conn().Query("SELECT method, path, handler_file, framework, "
                                   "COALESCE(file_id,0) FROM routes ORDER BY path");
        if (res->HasError()) return routes;
        for (duckdb::idx_t i = 0; i < res->RowCount(); i++) {
            RouteInfo r;
            r.method = res->GetValue(0, i).ToString();
            r.path = res->GetValue(1, i).ToString();
            r.handler_file = res->GetValue(2, i).ToString();
            r.framework = res->GetValue(3, i).ToString();
            r.file_id = res->GetValue<int64_t>(4, i);
            routes.push_back(r);
        }
    } catch (...) {
    }
    return routes;
}

} // namespace axon
