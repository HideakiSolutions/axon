#include "http_server.hpp"
#include "peer.hpp"
#include "../core/git.hpp"
#include "../core/registry.hpp"
#include "../core/capsule.hpp"
#include "../core/ccr.hpp"
#include "../core/dialogue.hpp"
#include "../core/telemetry.hpp"
#include <nlohmann/json.hpp>
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define close(s) closesocket(s)
#  ifdef _MSC_VER
     typedef int ssize_t;   // MinGW defines ssize_t; MSVC does not
#  endif
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#endif
#include <csignal>    // SIGINT, signal() — available on all platforms
#include <chrono>
#include <unordered_set>
#include <sstream>
#include <iostream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace axon::mcp {

static volatile bool g_running = true;

static void build_response(int fd, int status, const std::string& body,
                           const std::string& content_type = "application/json") {
    std::string status_text = (status == 200) ? "OK" : (status == 404 ? "Not Found" : "Bad Request");
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << status_text << "\r\n"
        << "Content-Type: " << content_type << "; charset=utf-8\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        << "Access-Control-Allow-Headers: Content-Type\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    std::string response = oss.str();
    send(fd, response.c_str(), response.size(), 0);
}

// Extrai path e query string de "GET /api/search?q=foo HTTP/1.1"
static void parse_request_line(const std::string& request,
                               std::string& method, std::string& path,
                               std::string& query, std::string& body) {
    std::istringstream ss(request);
    std::string line;
    std::getline(ss, line);
    std::istringstream ls(line);
    std::string full_path, version;
    ls >> method >> full_path >> version;

    auto q = full_path.find('?');
    if (q != std::string::npos) {
        path  = full_path.substr(0, q);
        query = full_path.substr(q + 1);
    } else {
        path  = full_path;
        query = "";
    }

    // Read headers to find Content-Length, then read body
    auto body_start = request.find("\r\n\r\n");
    if (body_start != std::string::npos)
        body = request.substr(body_start + 4);
}

static std::string get_query_param(const std::string& query, const std::string& key) {
    std::string prefix = key + "=";
    size_t pos = query.find(prefix);
    if (pos == std::string::npos) return "";
    size_t end = query.find('&', pos + prefix.size());
    return query.substr(pos + prefix.size(),
                        end == std::string::npos ? std::string::npos : end - pos - prefix.size());
}

// URL decode simples
static std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') { out += ' '; continue; }
        if (s[i] == '%' && i + 2 < s.size()) {
            int c = 0;
            sscanf(s.c_str() + i + 1, "%2x", &c);
            out += (char)c;
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::string web_index_html() {
    return R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Axon Web</title>
  <style>
    :root { color-scheme: light dark; font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }
    body { margin: 0; background: #0f172a; color: #e5e7eb; }
    header { display: flex; align-items: center; gap: 16px; padding: 14px 18px; border-bottom: 1px solid #243044; background: #111827; }
    h1 { margin: 0; font-size: 18px; font-weight: 650; }
    main { display: grid; grid-template-columns: minmax(280px, 360px) 1fr; min-height: calc(100vh - 58px); }
    aside { border-right: 1px solid #243044; padding: 14px; overflow: auto; }
    input, select, button { border: 1px solid #334155; background: #172033; color: #e5e7eb; border-radius: 6px; padding: 8px 10px; }
    button { cursor: pointer; }
    .toolbar { display: flex; gap: 8px; margin-left: auto; }
    .stats { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-bottom: 12px; }
    .metrics { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; margin-bottom: 12px; }
    .stat { border: 1px solid #263449; border-radius: 6px; padding: 9px; background: #111827; }
    .stat b { display: block; font-size: 18px; }
    .search { width: calc(100% - 22px); margin-bottom: 10px; }
    .list { display: grid; gap: 6px; }
    .item { border: 1px solid #263449; border-radius: 6px; padding: 8px; background: #101827; }
    .item strong { display: block; font-size: 13px; overflow-wrap: anywhere; }
    .item span { color: #9ca3af; font-size: 12px; overflow-wrap: anywhere; }
    .canvas { position: relative; overflow: hidden; background: #0b1020; }
    svg { width: 100%; height: 100%; min-height: calc(100vh - 58px); display: block; }
    line { stroke: #334155; stroke-width: 1; }
    circle { fill: #38bdf8; stroke: #e5e7eb; stroke-width: 1; cursor: pointer; }
    text { fill: #e5e7eb; font-size: 11px; paint-order: stroke; stroke: #0b1020; stroke-width: 3px; }
    .empty { color: #9ca3af; padding: 14px; }
    @media (max-width: 760px) { main { grid-template-columns: 1fr; } aside { border-right: 0; border-bottom: 1px solid #243044; max-height: 42vh; } }
  </style>
</head>
<body>
  <header>
    <h1>Axon Web</h1>
    <div class="toolbar">
      <select id="mode"><option value="file">Files</option><option value="symbol">Symbols</option></select>
      <button id="refresh">Refresh</button>
    </div>
  </header>
  <main>
    <aside>
      <div class="stats">
        <div class="stat"><b id="nodes">0</b><span>nodes</span></div>
        <div class="stat"><b id="edges">0</b><span>edges</span></div>
        <div class="stat"><b id="files">0</b><span>files</span></div>
      </div>
      <div class="metrics" id="metrics"></div>
      <input id="filter" class="search" placeholder="Filter nodes">
      <div id="list" class="list"></div>
    </aside>
    <section class="canvas"><svg id="graph" role="img" aria-label="Axon dependency graph"></svg></section>
  </main>
  <script>
    const graph = document.getElementById('graph');
    const list = document.getElementById('list');
    const mode = document.getElementById('mode');
    const filter = document.getElementById('filter');
    let current = { nodes: [], edges: [], meta: {} };

    function point(i, n) {
      const w = graph.clientWidth || 900, h = graph.clientHeight || 600;
      const r = Math.max(80, Math.min(w, h) * 0.38);
      const a = (Math.PI * 2 * i) / Math.max(n, 1);
      return { x: w / 2 + Math.cos(a) * r, y: h / 2 + Math.sin(a) * r };
    }

    function escapeHtml(value) {
      return String(value || '').replace(/[&<>"']/g, ch => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[ch]));
    }

    function render() {
      const q = filter.value.toLowerCase();
      const nodes = current.nodes.filter(n => (n.label || n.id || '').toLowerCase().includes(q));
      const ids = new Set(nodes.map(n => String(n.id)));
      const pos = new Map(nodes.map((n, i) => [String(n.id), point(i, nodes.length)]));
      graph.replaceChildren();
      graph.setAttribute('viewBox', `0 0 ${graph.clientWidth || 900} ${graph.clientHeight || 600}`);
      current.edges.filter(e => ids.has(String(e.source)) && ids.has(String(e.target))).forEach(e => {
        const a = pos.get(String(e.source)), b = pos.get(String(e.target));
        if (!a || !b) return;
        const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
        line.setAttribute('x1', a.x); line.setAttribute('y1', a.y);
        line.setAttribute('x2', b.x); line.setAttribute('y2', b.y);
        graph.appendChild(line);
      });
      nodes.forEach(n => {
        const p = pos.get(String(n.id));
        const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');
        const c = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
        c.setAttribute('cx', p.x); c.setAttribute('cy', p.y); c.setAttribute('r', Math.max(4, Math.min(14, Number(n.size || 4) + 3)));
        const t = document.createElementNS('http://www.w3.org/2000/svg', 'text');
        t.setAttribute('x', p.x + 10); t.setAttribute('y', p.y + 4); t.textContent = n.label || n.id;
        g.append(c, t); graph.appendChild(g);
      });
      list.innerHTML = nodes.slice(0, 120).map(n => `<div class="item"><strong>${escapeHtml(n.label || n.id)}</strong><span>${escapeHtml(n.path || n.kind || '')}</span></div>`).join('') || '<div class="empty">No nodes</div>';
    }

    function metricCard(label, value) {
      return `<div class="stat"><b>${escapeHtml(value)}</b><span>${escapeHtml(label)}</span></div>`;
    }

    async function loadMetrics() {
      const data = await fetch('/api/metrics').then(r => r.json());
      const el = document.getElementById('metrics');
      if (data.telemetry_enabled) {
        el.innerHTML = [
          metricCard('requests', data.requests || 0),
          metricCard('tokens sent', data.tokens_sent || 0),
          metricCard('tokens saved', data.tokens_saved || 0),
          metricCard('cache hit', Math.round((data.cache_hit_rate || 0) * 100) + '%')
        ].join('');
      } else {
        const g = data.graph || {};
        el.innerHTML = [
          metricCard('symbols', g.symbols || 0),
          metricCard('graph edges', g.edges || 0),
          metricCard('bytes', g.bytes_indexed || 0),
          metricCard('capsule tok', data.last_capsule_token_estimate || 0)
        ].join('');
      }
    }

    async function load() {
      const suffix = mode.value === 'symbol' ? '?mode=symbol' : '';
      const data = await fetch('/api/graph' + suffix).then(r => r.json());
      current = data;
      document.getElementById('nodes').textContent = data.nodes?.length || 0;
      document.getElementById('edges').textContent = data.edges?.length || 0;
      document.getElementById('files').textContent = data.meta?.files || data.nodes?.length || 0;
      loadMetrics().catch(() => {});
      render();
    }
    document.getElementById('refresh').onclick = load;
    mode.onchange = load;
    filter.oninput = render;
    addEventListener('resize', render);
    load().catch(err => { list.innerHTML = `<div class="empty">${err.message}</div>`; });
  </script>
</body>
</html>)HTML";
}

static std::string handle_request(const std::string& method, const std::string& path,
                                  const std::string& query, const std::string& body,
                                  ServerContext& ctx, const HttpConfig& cfg) {
    // Escape SQL strings
    auto sq = [](const std::string& s) {
        std::string out;
        for (char c : s) { if (c == '\'') out += '\''; out += c; }
        return out;
    };

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        return web_index_html();
    }

    // GET /api/metrics
    if (method == "GET" && path == "/api/metrics") {
        return axon::metrics_json(ctx.cfg, ctx.db.get()).dump();
    }

    // GET /api/graph
    if (method == "GET" && path == "/api/graph") {
        // Parse query params
        bool symbol_mode = false;
        {
            auto amp = query.find("mode=symbol");
            if (amp != std::string::npos) symbol_mode = true;
        }

        // ── SYMBOL MODE ────────────────────────────────────────────────────
        if (symbol_mode && ctx.db_ready()) {
            json nodes = json::array();
            json edges = json::array();

            // Nodes = symbols
            auto sr = ctx.db->conn().Query(
                "SELECT s.id, s.name, s.kind, f.path "
                "FROM symbols s JOIN files f ON s.file_id = f.id");
            if (!sr->HasError()) {
                for (duckdb::idx_t i = 0; i < sr->RowCount(); i++) {
                    std::string sid  = std::to_string(sr->GetValue<int64_t>(0, i));
                    std::string name = sr->GetValue(1, i).ToString();
                    std::string kind = sr->GetValue(2, i).ToString();
                    std::string fpath = sr->GetValue(3, i).ToString();
                    // Compute rough degree in edges below — use 1 for now, updated after edge scan
                    nodes.push_back({{"id", sid}, {"label", name}, {"kind", kind},
                                     {"path", fpath}, {"size", 1}});
                }
            }

            // Edges = symbol-to-symbol
            auto er = ctx.db->conn().Query(
                "SELECT CAST(e.from_symbol AS VARCHAR), CAST(e.to_symbol AS VARCHAR), e.kind "
                "FROM edges e "
                "WHERE e.from_symbol IS NOT NULL AND e.to_symbol IS NOT NULL");
            if (!er->HasError()) {
                // Count degrees
                std::map<std::string, int> deg;
                for (duckdb::idx_t i = 0; i < er->RowCount(); i++) {
                    deg[er->GetValue(0, i).ToString()]++;
                    deg[er->GetValue(1, i).ToString()]++;
                }
                // Update sizes
                for (auto& n : nodes) {
                    std::string id = n["id"];
                    if (deg.count(id)) n["size"] = deg[id];
                }
                // Reset and re-iterate for edges
                er = ctx.db->conn().Query(
                    "SELECT CAST(e.from_symbol AS VARCHAR), CAST(e.to_symbol AS VARCHAR), e.kind "
                    "FROM edges e "
                    "WHERE e.from_symbol IS NOT NULL AND e.to_symbol IS NOT NULL");
                if (!er->HasError()) {
                    for (duckdb::idx_t i = 0; i < er->RowCount(); i++) {
                        std::string from = er->GetValue(0, i).ToString();
                        std::string to   = er->GetValue(1, i).ToString();
                        std::string kind = er->GetValue(2, i).ToString();
                        edges.push_back({{"id", from + "->" + to},
                                         {"source", from}, {"target", to}, {"kind", kind}});
                    }
                }
            }

            std::unordered_set<std::string> node_files;
            for (const auto& n : nodes)
                if (n.contains("path")) node_files.insert(n["path"].get<std::string>());
            json meta = {
                {"files",   (int)node_files.size()},
                {"symbols", (int)nodes.size()},
                {"edges",   (int)edges.size()},
                {"project", ctx.cfg.project_root.filename().string()}
            };
            return json{{"nodes", nodes}, {"edges", edges}, {"meta", meta}}.dump();
        }

        // ── FILE MODE (default) ────────────────────────────────────────────
        json nodes = json::array();
        json edges = json::array();

        for (const auto& [id, fpath] : ctx.graph.id_to_path) {
            // Extract language from path extension
            std::string lang = "unknown";
            auto dot = fpath.find_last_of('.');
            if (dot != std::string::npos) lang = fpath.substr(dot + 1);

            int deg = ctx.graph.degree(id);
            std::string label = fpath.substr(fpath.find_last_of('/') + 1);
            nodes.push_back({{"id", fpath}, {"label", label}, {"language", lang},
                             {"path", fpath}, {"size", deg}, {"kind", "file"}});
        }

        // Prefer DB query to get symbol-granular edge info; fall back to in-memory
        if (ctx.db_ready()) {
            auto eres = ctx.db->conn().Query(
                "SELECT f1.path, f2.path, e.kind, s1.name, s2.name "
                "FROM edges e "
                "JOIN files f1 ON e.from_file = f1.id "
                "JOIN files f2 ON e.to_file   = f2.id "
                "LEFT JOIN symbols s1 ON e.from_symbol = s1.id "
                "LEFT JOIN symbols s2 ON e.to_symbol   = s2.id");
            if (!eres->HasError()) {
                for (duckdb::idx_t i = 0; i < eres->RowCount(); i++) {
                    std::string from = eres->GetValue(0, i).ToString();
                    std::string to   = eres->GetValue(1, i).ToString();
                    std::string kind = eres->GetValue(2, i).ToString();
                    json edge = {{"id", from + "->" + to}, {"source", from},
                                 {"target", to}, {"kind", kind}};
                    auto fsym = eres->GetValue(3, i);
                    auto tsym = eres->GetValue(4, i);
                    if (!fsym.IsNull()) edge["from_symbol"] = fsym.ToString();
                    if (!tsym.IsNull()) edge["to_symbol"]   = tsym.ToString();
                    edges.push_back(edge);
                }
            }
        } else {
            for (const auto& [from_id, targets] : ctx.graph.outgoing) {
                auto from_it = ctx.graph.id_to_path.find(from_id);
                if (from_it == ctx.graph.id_to_path.end()) continue;
                for (int64_t to_id : targets) {
                    auto to_it = ctx.graph.id_to_path.find(to_id);
                    if (to_it == ctx.graph.id_to_path.end()) continue;
                    edges.push_back({{"id", from_it->second + "->" + to_it->second},
                                     {"source", from_it->second},
                                     {"target", to_it->second},
                                     {"kind", "imports"}});
                }
            }
        }

        // Aggregate extra repos if --group or --all was specified
        std::vector<axon::RepoEntry> extra_repos;
        if (cfg.all_repos) {
            auto reg = axon::load_registry();
            extra_repos = axon::get_repos(reg);
        } else if (!cfg.group.empty()) {
            auto reg = axon::load_registry();
            extra_repos = axon::get_group_repos(reg, cfg.group);
        }

        for (auto& repo : extra_repos) {
            if (repo.db_path == ctx.cfg.db_path.string()) continue; // skip self
            try {
                duckdb::DBConfig db_cfg;
                db_cfg.options.access_mode = duckdb::AccessMode::READ_ONLY;
                duckdb::DuckDB other_db(repo.db_path, &db_cfg);
                duckdb::Connection other_conn(other_db);

                // Query nodes (degree = outgoing + incoming edges)
                auto res = other_conn.Query(
                    "SELECT f.path, "
                    "  (SELECT COUNT(*) FROM edges WHERE from_file = f.id) + "
                    "  (SELECT COUNT(*) FROM edges WHERE to_file   = f.id) AS degree "
                    "FROM files f");
                if (!res->HasError()) {
                    for (size_t i = 0; i < res->RowCount(); i++) {
                        std::string path = repo.name + "/" + res->GetValue(0, i).ToString();
                        int degree = 0;
                        try { degree = std::stoi(res->GetValue(1, i).ToString()); } catch (...) {}
                        std::string label = fs::path(path).filename().string();
                        nodes.push_back({{"id", path}, {"label", label}, {"degree", degree},
                                         {"path", path}, {"size", degree}, {"kind", "file"},
                                         {"repo", repo.name}});
                    }
                } else {
                    std::cerr << "[axon] skip repo " << repo.name << " nodes: " << res->GetError() << "\n";
                }

                // Query edges
                auto eres = other_conn.Query(
                    "SELECT f1.path, f2.path FROM edges e "
                    "JOIN files f1 ON e.from_file = f1.id "
                    "JOIN files f2 ON e.to_file   = f2.id");
                if (!eres->HasError()) {
                    for (size_t i = 0; i < eres->RowCount(); i++) {
                        std::string from = repo.name + "/" + eres->GetValue(0, i).ToString();
                        std::string to   = repo.name + "/" + eres->GetValue(1, i).ToString();
                        edges.push_back({{"id", from + "->" + to}, {"source", from}, {"target", to},
                                         {"kind", "imports"}, {"repo", repo.name}});
                    }
                } else {
                    std::cerr << "[axon] skip repo " << repo.name << " edges: " << eres->GetError() << "\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "[axon] skip repo " << repo.name << ": " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[axon] skip repo " << repo.name << ": unknown error\n";
            }
        }

        json meta = {
            {"files",   (int)ctx.graph.id_to_path.size()},
            {"symbols", 0},  // filled below if DB available
            {"edges",   (int)edges.size()},
            {"project", ctx.cfg.project_root.filename().string()}
        };

        if (ctx.db_ready()) {
            auto sr = ctx.db->conn().Query("SELECT count(*) FROM symbols");
            if (!sr->HasError() && sr->RowCount() > 0)
                meta["symbols"] = sr->GetValue<int64_t>(0, 0);
        }

        return json{{"nodes", nodes}, {"edges", edges}, {"meta", meta}}.dump();
    }

    // GET /api/overview
    if (method == "GET" && path == "/api/overview") {
        auto top = ctx.graph.top_files_by_degree(20);
        json top_files = json::array();
        for (const auto& n : top)
            top_files.push_back({{"path", n.path}, {"degree", n.degree}});

        json top_symbols = json::array();
        if (ctx.db_ready()) {
            auto sr = ctx.db->conn().Query(
                "SELECT s.name, s.kind, f.path FROM symbols s "
                "JOIN files f ON s.file_id = f.id LIMIT 20");
            if (!sr->HasError()) {
                for (duckdb::idx_t i = 0; i < sr->RowCount(); i++)
                    top_symbols.push_back({{"name", sr->GetValue(0,i).ToString()},
                                          {"kind", sr->GetValue(1,i).ToString()},
                                          {"file", sr->GetValue(2,i).ToString()}});
            }
        }
        return json{{"top_files", top_files}, {"top_symbols", top_symbols}}.dump();
    }

    // GET /api/search?q=<query>
    if (method == "GET" && path == "/api/search") {
        std::string q = url_decode(get_query_param(query, "q"));
        json files = json::array();
        json symbols = json::array();

        if (!q.empty() && ctx.db_ready()) {
            auto fr = ctx.db->conn().Query(
                "SELECT path, language FROM files WHERE path LIKE '%" + sq(q) + "%' LIMIT 20");
            if (!fr->HasError())
                for (duckdb::idx_t i = 0; i < fr->RowCount(); i++)
                    files.push_back({{"path", fr->GetValue(0,i).ToString()},
                                    {"language", fr->GetValue(1,i).ToString()}});

            auto sr = ctx.db->conn().Query(
                "SELECT s.name, s.kind, f.path, s.start_line FROM symbols s "
                "JOIN files f ON s.file_id = f.id "
                "WHERE s.name LIKE '%" + sq(q) + "%' LIMIT 20");
            if (!sr->HasError())
                for (duckdb::idx_t i = 0; i < sr->RowCount(); i++)
                    symbols.push_back({{"name", sr->GetValue(0,i).ToString()},
                                      {"kind", sr->GetValue(1,i).ToString()},
                                      {"file", sr->GetValue(2,i).ToString()},
                                      {"line", sr->GetValue(3,i).GetValue<int32_t>()}});
        }
        return json{{"files", files}, {"symbols", symbols}}.dump();
    }

    // GET /api/symbol/<name>
    if (method == "GET" && path.rfind("/api/symbol/", 0) == 0) {
        std::string sym_name = url_decode(path.substr(12));
        json result = json::object();

        if (!sym_name.empty() && ctx.db_ready()) {
            auto sr = ctx.db->conn().Query(
                "SELECT s.name, s.kind, f.path, s.start_line, s.signature, s.file_id "
                "FROM symbols s JOIN files f ON s.file_id = f.id "
                "WHERE s.name = '" + sq(sym_name) + "' LIMIT 1");
            if (!sr->HasError() && sr->RowCount() > 0) {
                int64_t file_id = sr->GetValue<int64_t>(5, 0);
                json caller_files = json::array();
                auto it = ctx.graph.incoming.find(file_id);
                if (it != ctx.graph.incoming.end())
                    for (int64_t src : it->second) {
                        auto pit = ctx.graph.id_to_path.find(src);
                        if (pit != ctx.graph.id_to_path.end())
                            caller_files.push_back(pit->second);
                    }
                result = {{"name",         sr->GetValue(0,0).ToString()},
                          {"kind",         sr->GetValue(1,0).ToString()},
                          {"file",         sr->GetValue(2,0).ToString()},
                          {"line",         sr->GetValue(3,0).GetValue<int32_t>()},
                          {"signature",    sr->GetValue(4,0).ToString()},
                          {"caller_files", caller_files}};
            }
        }
        return result.dump();
    }

    // POST /api/detect-changes
    if (method == "POST" && path == "/api/detect-changes") {
        std::string ref = "HEAD";
        try {
            if (!body.empty()) {
                auto b = json::parse(body);
                ref = b.value("ref", "HEAD");
            }
        } catch (...) {}

        std::string root = ctx.cfg.project_root.string();
        if (!axon::is_git_repo(root))
            return json{{"error","Not a git repository"}}.dump();

        auto diffs = axon::get_git_diffs(root, ref);
        json changed_files = json::array();
        json affected_symbols = json::array();

        for (const auto& diff : diffs) {
            changed_files.push_back(diff.path);
            if (!ctx.db_ready() || diff.hunks.empty()) continue;
            auto sq2 = [&](const std::string& s) -> std::string {
                std::string o; for (char c : s) { if (c=='\'') o+='\''; o+=c; } return o;
            };
            auto fid_res = ctx.db->conn().Query(
                "SELECT id FROM files WHERE path = '" + sq2(diff.path) + "'");
            if (fid_res->HasError() || fid_res->RowCount() == 0) continue;
            int64_t file_id = fid_res->GetValue<int64_t>(0, 0);
            for (const auto& hunk : diff.hunks) {
                auto sym_res = ctx.db->conn().Query(
                    "SELECT name, kind, start_line FROM symbols WHERE file_id = " +
                    std::to_string(file_id) +
                    " AND start_line <= " + std::to_string(hunk.end_line) +
                    " AND end_line >= " + std::to_string(hunk.start_line));
                if (sym_res->HasError()) continue;
                for (duckdb::idx_t i = 0; i < sym_res->RowCount(); i++)
                    affected_symbols.push_back({{"name", sym_res->GetValue(0,i).ToString()},
                                               {"kind", sym_res->GetValue(1,i).ToString()},
                                               {"file", diff.path},
                                               {"line", sym_res->GetValue(2,i).GetValue<int32_t>()}});
            }
        }
        return json{{"ref", ref}, {"changed_files", changed_files},
                   {"affected_symbols", affected_symbols}}.dump();
    }

    // GET /api/observations?q=<text>&limit=N
    if (method == "GET" && path == "/api/observations") {
        json obs = json::array();
        if (ctx.db_ready()) {
            std::string q = url_decode(get_query_param(query, "q"));
            std::string limit_str = get_query_param(query, "limit");
            int limit = limit_str.empty() ? 10 : std::stoi(limit_str);

            if (!q.empty() && ctx.model_ready()) {
                auto emb = ctx.model->embed(q);
                std::ostringstream vs;
                vs << "[";
                for (size_t i = 0; i < emb.size(); i++) { if (i) vs << ","; vs << emb[i]; }
                vs << "]";
                auto res = ctx.db->conn().Query(
                    "SELECT id, content, file_path, created_at FROM observations "
                    "WHERE embedding IS NOT NULL "
                    "ORDER BY array_cosine_similarity(embedding, " + vs.str() + "::FLOAT[768]) DESC "
                    "LIMIT " + std::to_string(limit));
                if (!res->HasError())
                    for (duckdb::idx_t i = 0; i < res->RowCount(); i++)
                        obs.push_back({{"id",         res->GetValue<int64_t>(0, i)},
                                      {"content",    res->GetValue(1, i).ToString()},
                                      {"file_path",  res->GetValue(2, i).ToString()},
                                      {"created_at", res->GetValue(3, i).ToString()}});
            } else {
                auto res = ctx.db->conn().Query(
                    "SELECT id, content, file_path, created_at FROM observations "
                    "ORDER BY created_at DESC LIMIT " + std::to_string(limit));
                if (!res->HasError())
                    for (duckdb::idx_t i = 0; i < res->RowCount(); i++)
                        obs.push_back({{"id",         res->GetValue<int64_t>(0, i)},
                                      {"content",    res->GetValue(1, i).ToString()},
                                      {"file_path",  res->GetValue(2, i).ToString()},
                                      {"created_at", res->GetValue(3, i).ToString()}});
            }
        }
        return json{{"observations", obs}, {"count", (int)obs.size()}}.dump();
    }

    // GET /api/capsule?q=<query>&budget=8000&pivots=file1.ts,file2.ts
    if (method == "GET" && path == "/api/capsule") {
        std::string q = url_decode(get_query_param(query, "q"));
        std::string budget_str = get_query_param(query, "budget");
        std::string pivots_param = url_decode(get_query_param(query, "pivots"));
        int budget = budget_str.empty() ? 8000 : std::stoi(budget_str);

        if (q.empty() || !ctx.db_ready())
            return json{{"error", "q parameter required and DB must be ready"}}.dump();

        std::vector<std::string> explicit_pivots;
        if (!pivots_param.empty()) {
            std::istringstream ps(pivots_param);
            std::string pivot;
            while (std::getline(ps, pivot, ','))
                if (!pivot.empty()) explicit_pivots.push_back(pivot);
        }

        auto capsule_to_json = [](const axon::ContextCapsule& c,
                                  const char* cache_state) -> std::string {
            json pivot_files = json::array();
            for (const auto& f : c.pivot_files)
                pivot_files.push_back({{"path", f.path}, {"content", f.content},
                                       {"source_ref", f.source_ref},
                                       {"expand_command", f.expand_command},
                                       {"is_skeleton", f.is_skeleton}, {"token_estimate", f.token_estimate}});
            json support_files = json::array();
            for (const auto& f : c.support_files)
                support_files.push_back({{"path", f.path}, {"content", f.content},
                                         {"source_ref", f.source_ref},
                                         {"expand_command", f.expand_command},
                                         {"is_skeleton", f.is_skeleton}, {"token_estimate", f.token_estimate}});
            json cap = {{"query",          c.query},
                        {"pivot_files",    pivot_files},
                        {"support_files",  support_files},
                        {"token_estimate", c.token_estimate},
                        {"compression", {{"input_tokens", c.compression_input_tokens},
                                         {"output_tokens", c.compression_output_tokens},
                                         {"tokens_saved", c.compression_tokens_saved}}},
                        {"ccr_artifact_ids", c.ccr_artifact_ids},
                        {"total_files",    c.total_files}};
            if (cache_state) cap["cache"] = cache_state;
            return json{{"capsule", cap}}.dump();
        };

        // Same cache policy as the MCP handler: only query-driven capsules are
        // eligible (explicit pivots steer assembly and must not reuse entries
        // generated for the implicit-pivot path). Hits don't need the model.
        const std::string epoch = axon::current_project_epoch(*ctx.db);
        const bool eligible_for_cache = explicit_pivots.empty();
        std::string cache_key;
        if (eligible_for_cache) {
            cache_key = axon::compute_capsule_cache_key(q, budget, epoch);
            if (auto hit = axon::capsule_cache_lookup(*ctx.db, cache_key, epoch))
                return capsule_to_json(*hit, "hit");
        }

        if (!ctx.model_ready())
            return json{{"error", "Embedding model not loaded. Run axon index with embeddings enabled."}}.dump();

        auto capsule = axon::assemble_capsule(q, explicit_pivots, *ctx.db, *ctx.model,
                                              ctx.graph, ctx.cfg.project_root, budget);
        if (eligible_for_cache)
            axon::capsule_cache_insert(*ctx.db, cache_key, epoch, capsule);

        return capsule_to_json(capsule, nullptr);
    }

    // GET /api/artifact/<artifact_id>
    if (method == "GET" && path.rfind("/api/artifact/", 0) == 0) {
        std::string artifact_id = url_decode(path.substr(std::string("/api/artifact/").size()));
        std::optional<axon::CcrArtifact> artifact;
        if (ctx.db_ready())
            artifact = axon::ccr_retrieve_artifact(*ctx.db, artifact_id);
        // Fall back to the file sidecar (populated by `axon filter` while
        // another process held the DB lock).
        if (!artifact)
            artifact = axon::ccr_retrieve_artifact_file(ctx.cfg.axon_dir / "ccr", artifact_id);
        if (!artifact) {
            if (!ctx.db_ready()) return json{{"error","DB not ready"}}.dump();
            return json{{"error","artifact not found"},{"artifact_id",artifact_id}}.dump();
        }
        return json{{"artifact", {{"artifact_id", artifact->artifact_id},
                                  {"kind", artifact->kind},
                                  {"source_ref", artifact->source_ref},
                                  {"content", artifact->content},
                                  {"token_estimate", artifact->token_estimate}}}}.dump();
    }

    // ── Dialogue Layer HTTP endpoints ─────────────────────────────────────────

    // GET /api/threads
    if (method == "GET" && path == "/api/threads") {
        if (!ctx.db_ready()) return json{{"error","DB not ready"}}.dump();
        auto threads = axon::thread_list(*ctx.db);
        json result = json::array();
        for (const auto& t : threads)
            result.push_back({{"id",t.id},{"name",t.name},{"kind",t.kind},{"created_at",t.created_at}});
        return json{{"threads", result}, {"count", (int)result.size()}}.dump();
    }

    // GET /api/threads/:id/sessions
    if (method == "GET" && path.rfind("/api/threads/", 0) == 0 &&
        path.find("/sessions") != std::string::npos) {
        if (!ctx.db_ready()) return json{{"error","DB not ready"}}.dump();
        auto slash = path.rfind('/', path.size() - 9);
        int64_t thread_id = std::stoll(path.substr(13, slash - 13));
        auto sessions = axon::thread_get_sessions(*ctx.db, thread_id);
        json result = json::array();
        for (const auto& s : sessions)
            result.push_back({{"id",s.id},{"label",s.label},
                              {"started_at",s.started_at},{"ended_at",s.ended_at},
                              {"digest",s.digest}});
        return json{{"sessions", result}}.dump();
    }

    // GET /api/sessions/:id/turns
    if (method == "GET" && path.rfind("/api/sessions/", 0) == 0 &&
        path.find("/turns") != std::string::npos) {
        if (!ctx.db_ready()) return json{{"error","DB not ready"}}.dump();
        int64_t session_id = std::stoll(path.substr(14, path.find("/turns") - 14));
        auto turns = axon::session_get(*ctx.db, session_id);
        json result = json::array();
        for (const auto& t : turns)
            result.push_back({{"id",t.id},{"role",t.role},{"content",t.content},{"ts",t.ts}});
        return json{{"turns", result}, {"session_id", session_id}}.dump();
    }

    // GET /api/dialogue/search?q=<query>&limit=N&thread_id=N
    if (method == "GET" && path == "/api/dialogue/search") {
        if (!ctx.db_ready() || !ctx.model_ready())
            return json{{"error","DB or model not ready"}}.dump();
        std::string q     = url_decode(get_query_param(query, "q"));
        std::string lim   = get_query_param(query, "limit");
        std::string tid_s = get_query_param(query, "thread_id");
        int limit         = lim.empty()   ? 5  : std::stoi(lim);
        int64_t thread_id = tid_s.empty() ? -1 : std::stoll(tid_s);
        auto hits = axon::turn_search(*ctx.db, *ctx.model, q, limit, thread_id);
        json result = json::array();
        for (const auto& h : hits)
            result.push_back({{"turn_id",h.turn.id},{"role",h.turn.role},
                              {"content",h.turn.content},{"ts",h.turn.ts},
                              {"session",h.session_label},{"thread",h.thread_name},
                              {"score",h.score}});
        return json{{"results", result}, {"count", (int)result.size()}}.dump();
    }

    return json{{"error","Not found"}}.dump();
}

void run_http(ServerContext& ctx, const HttpConfig& cfg) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    signal(SIGINT, [](int) { g_running = false; });
#ifndef _WIN32
    signal(SIGTERM, [](int) { g_running = false; });
#endif

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::cerr << "socket() failed\n"; return; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg.port);
    inet_pton(AF_INET, cfg.host.c_str(), &addr.sin_addr);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind() failed on " << cfg.host << ":" << cfg.port << "\n";
        close(server_fd);
        return;
    }
    listen(server_fd, 16);
    std::cout << "Axon Web listening on http://" << cfg.host << ":" << cfg.port << "\n";
    std::cout << "Endpoints: /api/graph  /api/overview  /api/search?q=  /api/symbol/<name>  /api/detect-changes  /api/observations  /api/capsule\n";

    // Same peer-proxy mechanism as stdio serves: a localhost listener that
    // executes tool calls for latecomer serves while this process holds the
    // DuckDB write lock (always on 127.0.0.1 even when --host is public).
    if (start_peer_listener(ctx) && ctx.db_ready())
        register_self_as_owner(ctx, ctx.peer_port);

    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        timeval tv{1, 0};  // 1s timeout to check g_running
        if (select(server_fd + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;

        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        // Read request (simple: read up to 8KB)
        char buf[8192] = {};
        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            std::string request(buf, n);
            std::string method, path, query, body;
            parse_request_line(request, method, path, query, body);

            std::string response_body;
            auto start = std::chrono::steady_clock::now();
            // Serialize against the peer listener thread, which may be
            // executing a proxied tool call on the same ctx right now.
            std::lock_guard<std::mutex> lock(*ctx.tool_mutex);
            if (method == "OPTIONS") {
                response_body = "";
            } else {
                response_body = handle_request(method, path, query, body, ctx, cfg);
            }
            if (path != "/api/metrics") {
                int64_t latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                int64_t tokens = static_cast<int64_t>(response_body.size() / 4);
                bool cache_hit = response_body.find("\"cache\":\"hit\"") != std::string::npos;
                axon::record_telemetry(ctx.cfg, ctx.db.get(), {
                    path, "http", latency_ms, tokens, tokens * 4, tokens * 3, cache_hit, ""
                });
            }
            std::string content_type = (path == "/" || path == "/index.html") ? "text/html" : "application/json";
            build_response(client_fd, 200, response_body, content_type);
        }
        close(client_fd);
    }

    unregister_self_as_owner(ctx);
    stop_peer_listener();

    close(server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    std::cout << "\nHTTP server stopped.\n";
}

} // namespace axon::mcp
