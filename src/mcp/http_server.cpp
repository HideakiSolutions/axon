#include "http_server.hpp"
#include "../core/git.hpp"
#include "../core/registry.hpp"
#include "../core/capsule.hpp"
#include <nlohmann/json.hpp>
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define close(s) closesocket(s)
   typedef int ssize_t;
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <signal.h>
#endif
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

static std::string handle_request(const std::string& method, const std::string& path,
                                  const std::string& query, const std::string& body,
                                  ServerContext& ctx, const HttpConfig& cfg) {
    // Escape SQL strings
    auto sq = [](const std::string& s) {
        std::string out;
        for (char c : s) { if (c == '\'') out += '\''; out += c; }
        return out;
    };

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

            json meta = {
                {"files",   (int)nodes.size()},
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

        if (!ctx.model_ready())
            return json{{"error", "Embedding model not loaded. Run axon index with embeddings enabled."}}.dump();

        auto capsule = axon::assemble_capsule(q, explicit_pivots, *ctx.db, *ctx.model,
                                              ctx.graph, ctx.cfg.project_root, budget);

        json pivot_files = json::array();
        for (const auto& f : capsule.pivot_files)
            pivot_files.push_back({{"path", f.path}, {"content", f.content},
                                   {"is_skeleton", f.is_skeleton}, {"token_estimate", f.token_estimate}});

        json support_files = json::array();
        for (const auto& f : capsule.support_files)
            support_files.push_back({{"path", f.path}, {"content", f.content},
                                     {"is_skeleton", f.is_skeleton}, {"token_estimate", f.token_estimate}});

        return json{{"capsule", {{"query",          capsule.query},
                                 {"pivot_files",    pivot_files},
                                 {"support_files",  support_files},
                                 {"token_estimate", capsule.token_estimate},
                                 {"total_files",    capsule.total_files}}}}.dump();
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
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

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
    std::cout << "Axon HTTP API listening on http://" << cfg.host << ":" << cfg.port << "\n";
    std::cout << "Endpoints: /api/graph  /api/overview  /api/search?q=  /api/symbol/<name>  /api/detect-changes  /api/observations  /api/capsule\n";

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
            if (method == "OPTIONS") {
                response_body = "";
            } else {
                response_body = handle_request(method, path, query, body, ctx, cfg);
            }
            build_response(client_fd, 200, response_body);
        }
        close(client_fd);
    }

    close(server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    std::cout << "\nHTTP server stopped.\n";
}

} // namespace axon::mcp
