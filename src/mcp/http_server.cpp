#include "http_server.hpp"
#include "../core/git.hpp"
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
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
                                  ServerContext& ctx) {
    // Escape SQL strings
    auto sq = [](const std::string& s) {
        std::string out;
        for (char c : s) { if (c == '\'') out += '\''; out += c; }
        return out;
    };

    // GET /api/graph
    if (method == "GET" && path == "/api/graph") {
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

    return json{{"error","Not found"}}.dump();
}

void run_http(ServerContext& ctx, const HttpConfig& cfg) {
    signal(SIGINT,  [](int) { g_running = false; });
    signal(SIGTERM, [](int) { g_running = false; });

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
    std::cout << "Endpoints: /api/graph  /api/overview  /api/search?q=  /api/symbol/<name>  /api/detect-changes\n";

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
                response_body = handle_request(method, path, query, body, ctx);
            }
            build_response(client_fd, 200, response_body);
        }
        close(client_fd);
    }

    close(server_fd);
    std::cout << "\nHTTP server stopped.\n";
}

} // namespace axon::mcp
