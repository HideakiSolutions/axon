#include "impact_query_adapter.hpp"
#include "db.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace axon {
namespace {

std::string trim(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string symbol_id(const std::string& repo, int64_t id) {
    return repo + ":" + std::to_string(id);
}

bool safe_relative_file(const std::filesystem::path& path) {
    if (path.is_absolute() || path.empty()) return false;
    for (const auto& component : path)
        if (component == "..") return false;
    return true;
}

std::string read_lines(const std::filesystem::path& root, const std::string& file,
                       int first, int last, std::size_t max_bytes) {
    const std::filesystem::path relative(file);
    if (!safe_relative_file(relative) || first <= 0 || last < first ||
        last - first > 2000) return {};
    std::ifstream input(root / relative);
    if (!input) return {};
    std::string line, body;
    for (int number = 1; number <= last && std::getline(input, line); ++number) {
        if (number < first) continue;
        if (body.size() + line.size() + 1 > max_bytes) return {};
        body += line + '\n';
    }
    return body;
}

const IndexedImpactSymbol* find_symbol(const ImpactIndexSnapshot& index,
                                        const ContractEvidence& evidence) {
    for (const auto& symbol : index.symbols)
        if (symbol.repository == evidence.repository && symbol.file == evidence.file &&
            symbol.name == evidence.symbol) return &symbol;
    return nullptr;
}

// Split an object literal into top-level properties. This rejects spreads,
// computed names and malformed delimiters instead of claiming a closed shape.
bool parse_object_fields(const std::string& source, std::size_t open,
                         std::set<std::string>& fields, std::size_t& end) {
    if (open >= source.size() || source[open] != '{') return false;
    int braces = 0, brackets = 0, parens = 0;
    char quote = 0;
    bool escaped = false;
    std::size_t start = open + 1;
    static const std::regex key_pattern(
        R"key(^\s*(?:([A-Za-z_$][A-Za-z0-9_$]*)|"([^"]+)"|'([^']+)')\s*:)key");
    auto collect = [&](std::size_t stop) {
        const auto property = trim(source.substr(start, stop - start));
        if (property.empty()) return true;
        if (property.rfind("...", 0) == 0 || property.front() == '[') return false;
        std::smatch match;
        if (!std::regex_search(property, match, key_pattern)) return false;
        for (int group = 1; group <= 3; ++group)
            if (match[group].matched) fields.insert(match[group].str());
        return true;
    };
    for (std::size_t i = open; i < source.size(); ++i) {
        const char ch = source[i];
        if (quote) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == quote) quote = 0;
            continue;
        }
        if (ch == '\'' || ch == '"' || ch == '`') { quote = ch; continue; }
        if (ch == '{') ++braces;
        else if (ch == '}') {
            --braces;
            if (braces == 0) {
                if (!collect(i)) return false;
                end = i + 1;
                return brackets == 0 && parens == 0;
            }
        } else if (ch == '[') ++brackets;
        else if (ch == ']') --brackets;
        else if (ch == '(') ++parens;
        else if (ch == ')') --parens;
        else if (ch == ',' && braces == 1 && brackets == 0 && parens == 0) {
            if (!collect(i)) return false;
            start = i + 1;
        }
        if (braces < 0 || brackets < 0 || parens < 0) return false;
    }
    return false;
}

std::set<std::string> accessed_fields(const std::string& body) {
    std::set<std::string> fields;
    static const std::regex access(
        R"(\b(?:response|result|resp)\s*\.\s*data\s*\.\s*([A-Za-z_$][A-Za-z0-9_$]*))");
    for (std::sregex_iterator it(body.begin(), body.end(), access), end; it != end; ++it)
        fields.insert((*it)[1].str());
    return fields;
}

} // namespace

ImpactIndexSnapshot load_impact_index(duckdb::Connection& conn,
                                      const std::filesystem::path& project_root,
                                      const std::string& repository,
                                      const ImpactLoadLimits& limits) {
    ImpactIndexSnapshot snapshot;
    auto rows = conn.Query("SELECT s.id,f.path,s.name,s.start_line,s.end_line "
                           "FROM symbols s JOIN files f ON f.id=s.file_id "
                           "ORDER BY s.id LIMIT " + std::to_string(limits.max_symbols + 1));
    require_ok(rows, "hydrate impact symbols");
    snapshot.truncated = rows->RowCount() > limits.max_symbols;
    std::set<int64_t> loaded;
    for (duckdb::idx_t row = 0; row < rows->RowCount() && row < limits.max_symbols; ++row) {
        const int64_t id = rows->GetValue<int64_t>(0, row);
        const std::string file = rows->GetValue(1, row).ToString();
        snapshot.symbols.push_back({symbol_id(repository, id), repository, file,
                                    rows->GetValue(2, row).ToString(),
                                    read_lines(project_root, file,
                                               rows->GetValue<int32_t>(3, row),
                                               rows->GetValue<int32_t>(4, row),
                                               limits.max_body_bytes)});
        loaded.insert(id);
    }
    auto edges = conn.Query("SELECT from_symbol,to_symbol,kind FROM edges "
                            "WHERE from_symbol IS NOT NULL AND to_symbol IS NOT NULL "
                            "AND kind IN ('calls','imports') ORDER BY id LIMIT " +
                            std::to_string(limits.max_edges + 1));
    require_ok(edges, "hydrate impact relations");
    snapshot.truncated |= edges->RowCount() > limits.max_edges;
    for (duckdb::idx_t row = 0; row < edges->RowCount() && row < limits.max_edges; ++row) {
        const auto from = edges->GetValue<int64_t>(0, row);
        const auto to = edges->GetValue<int64_t>(1, row);
        if (!loaded.count(from) || !loaded.count(to)) continue;
        snapshot.relations.push_back({symbol_id(repository, from), symbol_id(repository, to),
                                      edges->GetValue(2, row).ToString(), true});
    }
    return snapshot;
}

ResponseShape hydrate_http_response_shape(const ContractLink& link,
                                          const ImpactIndexSnapshot& provider_index,
                                          const ImpactIndexSnapshot& consumer_index) {
    ResponseShape shape;
    if (link.provider.surface != "http" && link.provider.surface != "openapi") return shape;
    shape.link_identity = link.identity;
    shape.provider_symbol = link.provider.repository + "/" + link.provider.file + "::" +
                            link.provider.symbol;
    shape.consumer_symbol = link.consumer.repository + "/" + link.consumer.file + "::" +
                            link.consumer.symbol;
    const auto* provider = find_symbol(provider_index, link.provider);
    const auto* consumer = find_symbol(consumer_index, link.consumer);
    if (consumer) {
        for (const auto& field : accessed_fields(consumer->body))
            shape.accesses.push_back({field, "observed", true, false});
        if (std::regex_search(consumer->body,
                              std::regex(R"(\b(?:response|result|resp)\s*\.\s*data\s*\[)")))
            shape.accesses.push_back({"", "observed", true, true});
    }
    if (!provider || provider->body.empty()) return shape;
    const std::string& body = provider->body;
    static const std::regex response_call(R"(\b(?:res|reply|response)(?:\s*\.\s*status\([^)]*\))?\s*\.\s*json\s*\()");
    std::set<std::string> common, observed;
    bool first = true, complete = true;
    for (std::sregex_iterator it(body.begin(), body.end(), response_call), end; it != end; ++it) {
        std::size_t start = static_cast<std::size_t>(it->position() + it->length());
        while (start < body.size() && std::isspace(static_cast<unsigned char>(body[start]))) ++start;
        std::set<std::string> fields;
        std::size_t after = start;
        if (!parse_object_fields(body, start, fields, after)) {
            complete = false;
            continue;
        }
        observed.insert(fields.begin(), fields.end());
        if (first) { common = std::move(fields); first = false; }
        else {
            std::set<std::string> intersection;
            std::set_intersection(common.begin(), common.end(), fields.begin(), fields.end(),
                                  std::inserter(intersection, intersection.begin()));
            common = std::move(intersection);
        }
    }
    // A source body can contain other return paths that do not invoke json.
    // Treat them as unknown unless the only returns are the literal res.json calls.
    const bool suspicious_return =
        std::regex_search(body, std::regex(R"(\breturn\b)"));
    const bool other_response = std::regex_search(
        body, std::regex(R"(\b(?:res|reply|response)\s*\.\s*(?:send|end|redirect|render|sendStatus)\s*\()"));
    shape.closed = !first && complete && !suspicious_return && !other_response;
    if (shape.closed) {
    for (const auto& field : common) shape.guaranteed_fields.push_back({field, "observed"});
    for (const auto& field : observed) shape.observed_fields.push_back({field, "observed"});
        shape.closure_origin = "observed";
    }
    return shape;
}

ExecutionGraphInput hydrate_execution_graph(const ImpactIndexSnapshot& index,
                                            const std::vector<ContractEvidence>& evidence) {
    ExecutionGraphInput graph;
    std::map<std::string, ExecutionNode> nodes;
    for (const auto& symbol : index.symbols) nodes[symbol.id] = {symbol.id, "call", ""};
    for (const auto& item : evidence) {
        if (item.identity.empty() || item.ambiguity != "resolved") continue;
        const auto* symbol = find_symbol(index, item);
        if (!symbol) continue;
        auto& node = nodes[symbol->id];
        if (item.surface == "http" && item.role == "provider") node.kind = "http_handler";
        else if (item.surface == "grpc" && item.role == "provider") node.kind = "rpc_handler";
        else if (item.surface == "topic" && item.role == "consumer") node.kind = "event_handler";
    }
    for (const auto& relation : index.relations) {
        if (!relation.resolved || !nodes.count(relation.from) || !nodes.count(relation.to)) continue;
        graph.edges.push_back({relation.from, relation.to, relation.kind, "inferred"});
    }
    const std::vector<std::pair<std::string, std::regex>> sinks = {
        {"persistence", std::regex(R"(\b(?:save|insert|persist|upsert)\s*\()")},
        {"external_http", std::regex(R"(\b(?:fetch|axios|httpx|requests)\s*(?:\.|\())")},
        {"publish", std::regex(R"(\b(?:publish|emit|sendMessage)\s*\()")},
        {"return", std::regex(R"(\b(?:res|reply|response)\s*\.\s*json\s*\()")}};
    for (const auto& symbol : index.symbols) {
        if (symbol.body.empty()) continue;
        for (const auto& [kind, pattern] : sinks) {
            if (!std::regex_search(symbol.body, pattern)) continue;
            const std::string output_id = symbol.id + "#output:" + kind;
            nodes[output_id] = {output_id, "output", kind};
            graph.edges.push_back({symbol.id, output_id, "output", "inferred"});
        }
    }
    for (auto& [_, node] : nodes) graph.nodes.push_back(std::move(node));
    return graph;
}

} // namespace axon
