#include "server.hpp"
#include "../mcp/protocol.hpp"
#include "version.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string sql_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out;
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            out += static_cast<char>(std::strtol(hex, nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string uri_to_path(const std::string& uri) {
    const std::string prefix = "file://";
    if (uri.rfind(prefix, 0) == 0) return url_decode(uri.substr(prefix.size()));
    return uri;
}

std::string path_to_uri(const fs::path& path) {
    std::string value = fs::absolute(path).generic_string();
    return "file://" + value;
}

std::string db_path_for_uri(const axon::mcp::ServerContext& ctx, const std::string& uri) {
    fs::path absolute = fs::weakly_canonical(uri_to_path(uri));
    std::error_code ec;
    fs::path rel = fs::relative(absolute, ctx.cfg.project_root, ec);
    std::string rel_text = rel.generic_string();
    if (!ec && !rel_text.empty() && rel_text.rfind("..", 0) != 0)
        return rel_text;
    return absolute.generic_string();
}

int symbol_kind(const std::string& kind) {
    if (kind == "class" || kind == "struct") return 5;
    if (kind == "method") return 6;
    if (kind == "constructor") return 9;
    if (kind == "enum") return 10;
    if (kind == "interface" || kind == "protocol") return 11;
    if (kind == "function") return 12;
    if (kind == "variable") return 13;
    if (kind == "constant") return 14;
    return 12;
}

json range_for_lines(int64_t start_line, int64_t end_line) {
    int start = static_cast<int>(std::max<int64_t>(start_line - 1, 0));
    int end = static_cast<int>(std::max<int64_t>(end_line, start + 1));
    return {
        {"start", {{"line", start}, {"character", 0}}},
        {"end", {{"line", end}, {"character", 0}}}
    };
}

json location(const axon::mcp::ServerContext& ctx, const std::string& file_path,
              int64_t start_line, int64_t end_line) {
    return {
        {"uri", path_to_uri(ctx.cfg.project_root / file_path)},
        {"range", range_for_lines(start_line, end_line)}
    };
}

std::optional<json> symbol_at(const axon::mcp::ServerContext& ctx,
                              const std::string& uri, int64_t line_zero_based) {
    if (!ctx.db_ready()) return std::nullopt;
    std::string fpath = sql_escape(db_path_for_uri(ctx, uri));
    int64_t line = line_zero_based + 1;
    auto res = ctx.db->conn().Query(
        "SELECT s.id, s.name, s.kind, s.start_line, s.end_line, f.path "
        "FROM symbols s JOIN files f ON s.file_id = f.id "
        "WHERE f.path = '" + fpath + "' "
        "AND s.start_line <= " + std::to_string(line) + " "
        "AND s.end_line >= " + std::to_string(line) + " "
        "ORDER BY (s.end_line - s.start_line) ASC, s.id ASC LIMIT 1");
    if (res->HasError() || res->RowCount() == 0) return std::nullopt;
    return json{
        {"id", res->GetValue<int64_t>(0, 0)},
        {"name", res->GetValue(1, 0).ToString()},
        {"kind", res->GetValue(2, 0).ToString()},
        {"start_line", res->GetValue<int32_t>(3, 0)},
        {"end_line", res->GetValue<int32_t>(4, 0)},
        {"path", res->GetValue(5, 0).ToString()}
    };
}

bool read_message(std::istream& in, json& payload) {
    std::string line;
    size_t content_length = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const std::string header = "Content-Length:";
        if (line.rfind(header, 0) == 0) {
            content_length = static_cast<size_t>(std::stoul(line.substr(header.size())));
        }
    }
    if (content_length == 0) return false;

    std::string body(content_length, '\0');
    in.read(body.data(), static_cast<std::streamsize>(content_length));
    if (static_cast<size_t>(in.gcount()) != content_length) return false;

    try {
        payload = json::parse(body);
        return true;
    } catch (...) {
        payload = axon::mcp::make_error(nullptr, axon::mcp::PARSE_ERROR, "Parse error");
        return true;
    }
}

void write_message(const json& payload) {
    std::string body = payload.dump();
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

json initialize_result() {
    return {
        {"capabilities", {
            {"textDocumentSync", 1},
            {"workspaceSymbolProvider", true},
            {"documentSymbolProvider", true},
            {"definitionProvider", true},
            {"referencesProvider", true}
        }},
        {"serverInfo", {{"name", "axon-lsp"}, {"version", axon::VERSION}}}
    };
}

json workspace_symbol(axon::mcp::ServerContext& ctx, const json& params) {
    if (!ctx.db_ready()) return json::array();
    std::string query = sql_escape(params.value("query", ""));
    auto res = ctx.db->conn().Query(
        "SELECT s.name, s.kind, s.start_line, s.end_line, f.path "
        "FROM symbols s JOIN files f ON s.file_id = f.id "
        "WHERE s.name ILIKE '%" + query + "%' "
        "ORDER BY s.name LIMIT 50");
    json out = json::array();
    if (res->HasError()) return out;
    for (duckdb::idx_t i = 0; i < res->RowCount(); i++) {
        std::string name = res->GetValue(0, i).ToString();
        std::string kind = res->GetValue(1, i).ToString();
        int64_t start = res->GetValue<int32_t>(2, i);
        int64_t end = res->GetValue<int32_t>(3, i);
        std::string path = res->GetValue(4, i).ToString();
        out.push_back({
            {"name", name},
            {"kind", symbol_kind(kind)},
            {"location", location(ctx, path, start, end)}
        });
    }
    return out;
}

json document_symbol(axon::mcp::ServerContext& ctx, const json& params) {
    if (!ctx.db_ready()) return json::array();
    std::string uri = params.value("textDocument", json::object()).value("uri", "");
    std::string fpath = sql_escape(db_path_for_uri(ctx, uri));
    auto res = ctx.db->conn().Query(
        "SELECT name, kind, start_line, end_line FROM symbols "
        "WHERE file_id = (SELECT id FROM files WHERE path = '" + fpath + "' LIMIT 1) "
        "ORDER BY start_line, name");
    json out = json::array();
    if (res->HasError()) return out;
    for (duckdb::idx_t i = 0; i < res->RowCount(); i++) {
        std::string name = res->GetValue(0, i).ToString();
        std::string kind = res->GetValue(1, i).ToString();
        int64_t start = res->GetValue<int32_t>(2, i);
        int64_t end = res->GetValue<int32_t>(3, i);
        json range = range_for_lines(start, end);
        out.push_back({
            {"name", name},
            {"kind", symbol_kind(kind)},
            {"range", range},
            {"selectionRange", range}
        });
    }
    return out;
}

json definition(axon::mcp::ServerContext& ctx, const json& params) {
    auto text_doc = params.value("textDocument", json::object());
    auto pos = params.value("position", json::object());
    auto sym = symbol_at(ctx, text_doc.value("uri", ""), pos.value("line", 0));
    if (!sym) return nullptr;
    return location(ctx, sym->value("path", ""), sym->value("start_line", 1), sym->value("end_line", 1));
}

json references(axon::mcp::ServerContext& ctx, const json& params) {
    auto text_doc = params.value("textDocument", json::object());
    auto pos = params.value("position", json::object());
    auto sym = symbol_at(ctx, text_doc.value("uri", ""), pos.value("line", 0));
    if (!sym || !ctx.db_ready()) return json::array();

    std::string name = sql_escape(sym->value("name", ""));
    auto res = ctx.db->conn().Query(
        "SELECT f.path, s.start_line, s.end_line "
        "FROM symbols s JOIN files f ON s.file_id = f.id "
        "WHERE s.name = '" + name + "' ORDER BY f.path, s.start_line LIMIT 100");
    json out = json::array();
    if (res->HasError()) return out;
    for (duckdb::idx_t i = 0; i < res->RowCount(); i++) {
        out.push_back(location(ctx, res->GetValue(0, i).ToString(),
                               res->GetValue<int32_t>(1, i),
                               res->GetValue<int32_t>(2, i)));
    }
    return out;
}

json handle(axon::mcp::ServerContext& ctx, const json& req) {
    json id = req.contains("id") ? req["id"] : json(nullptr);
    std::string method = req.value("method", "");
    json params = req.value("params", json::object());

    if (method == "initialize")
        return axon::mcp::make_response(id, initialize_result());
    if (method == "shutdown")
        return axon::mcp::make_response(id, nullptr);
    if (method == "workspace/symbol")
        return axon::mcp::make_response(id, workspace_symbol(ctx, params));
    if (method == "textDocument/documentSymbol")
        return axon::mcp::make_response(id, document_symbol(ctx, params));
    if (method == "textDocument/definition")
        return axon::mcp::make_response(id, definition(ctx, params));
    if (method == "textDocument/references")
        return axon::mcp::make_response(id, references(ctx, params));

    return axon::mcp::make_error(id, axon::mcp::METHOD_NOT_FOUND, "Method not found: " + method);
}

} // namespace

namespace axon::lsp {

void run_stdio(axon::mcp::ServerContext& ctx) {
    json req;
    while (read_message(std::cin, req)) {
        std::string method = req.value("method", "");
        bool has_id = req.contains("id") && !req["id"].is_null();
        if (method == "exit") break;
        if (!has_id) continue;
        write_message(handle(ctx, req));
    }
}

} // namespace axon::lsp
