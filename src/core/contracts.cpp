#include "contracts.hpp"
#include "db.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace axon {
namespace fs = std::filesystem;

namespace {
std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}
std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n'\"");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n'\"");
    return value.substr(first, last - first + 1);
}

bool match(const std::string& text, const std::string& pattern, std::smatch& found,
           bool insensitive = false) {
    auto flags = std::regex::ECMAScript;
    if (insensitive) flags |= std::regex::icase;
    return std::regex_search(text, found, std::regex(pattern, flags));
}

std::string host_service(const std::string& url) {
    std::smatch m;
    if (!match(url, R"(https?://([A-Za-z0-9_-]+)(?:\.[A-Za-z0-9_.-]+)?(?::[0-9]+)?(?:/|$))", m))
        return {};
    return lower(m[1]);
}

std::string function_near(const std::string& source, size_t offset, const std::string& fallback) {
    // Source-level attribution is deliberately conservative: only a lexical
    // function declaration before the occurrence in this file is accepted.
    std::string prefix = source.substr(0, offset);
    std::regex re(
        R"((?:function\s+|def\s+|func\s+|(?:public|private|export|async|static)\s+(?:(?:async|static|public|private)\s+)*(?:(?:[A-Za-z_][A-Za-z0-9_<>,?\[\].]*|void)\s+)?)([A-Za-z_][A-Za-z0-9_]*)\s*\()",
        std::regex::ECMAScript);
    std::string last;
    for (std::sregex_iterator it(prefix.begin(), prefix.end(), re), end; it != end; ++it)
        last = (*it)[1];
    return last.empty() ? fallback : last;
}

void emit(std::vector<ContractEvidence>& out, const std::string& repo, const std::string& file,
          const std::string& symbol, const std::string& surface, const std::string& role,
          const std::string& identity, const std::string& origin, const std::string& detail = {}) {
    out.push_back({repo, file, symbol, surface, role, identity, origin,
                   identity.empty() ? "unknown" : "resolved", detail});
}

void extract_openapi(const std::string& source, const std::string& repo, const std::string& file,
                     std::vector<ContractEvidence>& out) {
    if (source.find("openapi:") == std::string::npos) return;
    std::istringstream lines(source);
    std::string line, route, method;
    std::regex route_re(R"(^\s{2}(/[^:]+):\s*$)");
    std::regex method_re(R"(^\s{4}(get|post|put|patch|delete|head|options):\s*$)",
                         std::regex::icase);
    std::regex operation_re(R"(^\s{6}operationId:\s*['\"]?([A-Za-z_][A-Za-z0-9_.-]*))");
    std::smatch m;
    while (std::getline(lines, line)) {
        if (std::regex_search(line, m, route_re)) route = m[1];
        if (std::regex_search(line, m, method_re)) method = lower(m[1]);
        if (!route.empty() && !method.empty() && std::regex_search(line, m, operation_re)) {
            emit(out, repo, file, m[1], "openapi", "declaration", repo + "|" + m[1].str(),
                 "declared", method + " " + route);
            emit(out, repo, file, m[1], "http", "declaration",
                 repo + "|" + upper(method) + "|" + route, "declared", "operationId=" + m[1].str());
            method.clear();
        }
    }
}

void extract_proto(const std::string& source, const std::string& repo, const std::string& file,
                   std::vector<ContractEvidence>& out) {
    std::smatch m;
    if (!match(source, R"(\bpackage\s+([A-Za-z0-9_.]+)\s*;)", m)) return;
    const std::string package = m[1];
    std::regex service_re(R"(\bservice\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{([^}]*)\})");
    for (std::sregex_iterator it(source.begin(), source.end(), service_re), end; it != end; ++it) {
        std::string service = (*it)[1], block = (*it)[2];
        std::regex rpc_re(R"(\brpc\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()");
        for (std::sregex_iterator rpc(block.begin(), block.end(), rpc_re), rpc_end; rpc != rpc_end;
             ++rpc)
            emit(out, repo, file, (*rpc)[1], "grpc", "declaration",
                 package + "|" + service + "|" + (*rpc)[1].str(), "declared");
    }
}

void extract_asyncapi(const std::string& source, const std::string& repo, const std::string& file,
                      std::vector<ContractEvidence>& out) {
    if (source.find("asyncapi:") == std::string::npos) return;
    std::regex operation_re(R"(^\s{2}([A-Za-z_][A-Za-z0-9_]*):\s*\{action:\s*(send|receive))",
                            std::regex::icase | std::regex::multiline);
    auto unknown_operations = [&] {
        for (std::sregex_iterator it(source.begin(), source.end(), operation_re), end; it != end;
             ++it)
            emit(out, repo, file, (*it)[1], "topic", "declaration", "", "declared",
                 "unresolved server or channel");
    };
    std::smatch host, protocol, address;
    const std::regex hosts(R"(\bhost:\s*['\"]?([A-Za-z0-9_.:-]+))");
    const std::regex protocols(R"(\bprotocol:\s*['\"]?([A-Za-z0-9_-]+))");
    const std::regex addresses(R"(\baddress:\s*['\"]?([A-Za-z0-9_.:/-]+))");
    // A single server/channel is unambiguous. Multi-server/channel documents
    // require resolving operation bindings; keep them unknown for this pilot.
    if (std::distance(std::sregex_iterator(source.begin(), source.end(), hosts),
                      std::sregex_iterator()) != 1 ||
        std::distance(std::sregex_iterator(source.begin(), source.end(), protocols),
                      std::sregex_iterator()) != 1 ||
        std::distance(std::sregex_iterator(source.begin(), source.end(), addresses),
                      std::sregex_iterator()) != 1 ||
        !std::regex_search(source, host, hosts) ||
        !std::regex_search(source, protocol, protocols) ||
        !std::regex_search(source, address, addresses)) {
        unknown_operations();
        return;
    }
    const std::string key = lower(protocol[1]) + "|" + lower(host[1]) + "|" + address[1].str();
    std::smatch channel;
    if (!match(source, R"(\bchannels:\s*\n\s{2}([A-Za-z_][A-Za-z0-9_]*):\s*\{address:)", channel)) {
        unknown_operations();
        return;
    }
    for (std::sregex_iterator it(source.begin(), source.end(), operation_re), end; it != end;
         ++it) {
        auto line_end = source.find('\n', (*it).position());
        const std::string line = source.substr((*it).position(), line_end - (*it).position());
        if (line.find("#/channels/" + channel[1].str()) == std::string::npos) {
            emit(out, repo, file, (*it)[1], "topic", "declaration", "", "declared",
                 "unresolved channel reference");
            continue;
        }
        emit(out, repo, file, (*it)[1], "topic", "declaration", key, "declared",
             lower((*it)[2]) == "send" ? "provider" : "consumer");
    }
}

void extract_source(
    const std::string& source, const std::string& repo, const std::string& file,
    std::vector<ContractEvidence>& out,
    const std::unordered_map<std::string, std::vector<std::string>>& proto_methods) {
    std::smatch m;
    auto rpc_identity = [&](const std::string& service, const std::string& method) {
        const auto it = proto_methods.find(method);
        if (it == proto_methods.end()) return std::string{};
        std::string resolved;
        for (const auto& identity : it->second) {
            if (identity.find("|" + service + "|" + method) == std::string::npos) continue;
            if (!resolved.empty()) return std::string{}; // multiple packages
            resolved = identity;
        }
        return resolved;
    };
    // Express route registration; method and path are literal.
    std::regex express_re(
        R"(\b(?:app|router)\.(get|post|put|patch|delete)\s*\(\s*['\"](/[^'\"]+)['\"]\s*,\s*(?:async\s+)?function\s+([A-Za-z_][A-Za-z0-9_]*))",
        std::regex::icase);
    for (std::sregex_iterator it(source.begin(), source.end(), express_re), end; it != end; ++it)
        emit(out, repo, file, (*it)[3], "http", "provider",
             repo + "|" + upper((*it)[1]) + "|" + (*it)[2].str(), "observed");

    // HTTP clients with a literal URL, or one constant base URL plus literal path.
    std::string base;
    if (match(source, R"(\b(?:ORDERS|BASE_URL|API_URL)\s*=\s*['\"](https?://[^'\"]+)['\"])", m))
        base = m[1];
    std::regex http_re(
        R"(\b(?:httpx\.|requests\.|http\.|client\.)(get|post|put|patch|delete)\s*\(\s*([fF]?['\"][^'\"]+['\"]|[A-Za-z_][A-Za-z0-9_]*))",
        std::regex::icase);
    for (std::sregex_iterator it(source.begin(), source.end(), http_re), end; it != end; ++it) {
        std::string raw = (*it)[2], url = trim(raw);
        if (url.find("{ORDERS}") != std::string::npos && !base.empty())
            url.replace(url.find("{ORDERS}"), 8, base);
        std::string host = host_service(url), route;
        if (!host.empty()) {
            std::smatch path;
            if (match(url, R"(https?://[^/]+(/[^?#]*))", path)) route = path[1];
        }
        emit(out, repo, file, function_near(source, (*it).position(), "http_call"), "http",
             "consumer",
             host.empty() || route.empty() ? "" : host + "|" + upper((*it)[1]) + "|" + route,
             "observed", raw);
    }
    if (source.find("GetAsync(route)") != std::string::npos)
        emit(out, repo, file, "Call", "http", "consumer", "", "observed", "dynamic route");

    // gRPC generated clients; only match when the method is declared in a proto
    // contract discoverable in the repository set. Unknown packages stay unknown.
    std::regex grpc_re(R"(\b([A-Za-z_][A-Za-z0-9_]*)\.([A-Z][A-Za-z0-9_]*)Async\s*\()");
    std::regex typed_client_re(
        R"(\b([A-Za-z_][A-Za-z0-9_]*)\.\1Client\s+([A-Za-z_][A-Za-z0-9_]*)\b)");
    std::regex any_client_re(R"(\b([A-Za-z_][A-Za-z0-9_.]*)Client\s+([A-Za-z_][A-Za-z0-9_]*)\b)");
    std::unordered_map<std::string, std::string> client_types;
    std::unordered_map<std::string, std::string> all_types;
    for (std::sregex_iterator it(source.begin(), source.end(), any_client_re), end; it != end;
         ++it) {
        const std::string scope = function_near(source, (*it).position(), "<file>");
        const std::string key = scope + "|" + (*it)[2].str();
        const std::string type = (*it)[1];
        auto [known, inserted] = all_types.emplace(key, type);
        if (!inserted && known->second != type) known->second.clear();
    }
    for (std::sregex_iterator it(source.begin(), source.end(), typed_client_re), end; it != end;
         ++it) {
        const std::string scope = function_near(source, (*it).position(), "<file>");
        const std::string variable = scope + "|" + (*it)[2].str();
        const std::string service = (*it)[1];
        auto [known, inserted] = client_types.emplace(variable, service);
        if (!inserted && known->second != service) known->second.clear();
    }
    for (std::sregex_iterator it(source.begin(), source.end(), grpc_re), end; it != end; ++it) {
        const std::string scope = function_near(source, (*it).position(), "<file>");
        const std::string receiver = scope + "|" + (*it)[1].str();
        const auto declared = client_types.find(receiver);
        const auto typed = all_types.find(receiver);
        if (declared == client_types.end() || declared->second.empty() ||
            typed == all_types.end() || typed->second != declared->second + "." + declared->second)
            continue;
        const std::string method = (*it)[2];
        const std::string service = declared->second;
        const std::string resolved = rpc_identity(service, method);
        emit(out, repo, file, function_near(source, (*it).position(), method), "grpc", "consumer",
             !resolved.empty() ? resolved : (service.empty() ? "" : service + "|" + method),
             "observed", method);
        if (resolved.empty()) out.back().ambiguity = "unknown";
    }
    if (file.size() >= 5 && file.substr(file.size() - 5) == ".java") {
        std::smatch class_name;
        if (!match(source, R"(\bclass\s+([A-Za-z_][A-Za-z0-9_]*))", class_name)) return;
        std::regex provider_re(R"(\bpublic\s+[A-Za-z_][A-Za-z0-9_]*\s+([a-z][A-Za-z0-9_]*)\s*\()");
        for (std::sregex_iterator it(source.begin(), source.end(), provider_re), end; it != end;
             ++it) {
            std::string method = (*it)[1];
            method[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(method[0])));
            const std::string resolved = rpc_identity(class_name[1], method);
            if (!resolved.empty())
                emit(out, repo, file, (*it)[1], "grpc", "provider", resolved, "observed");
        }
    }

    // Kafka/static topic usage. Require a broker value; topic alone is never
    // sufficient because identical names can exist on different brokers.
    std::string broker, topic;
    if (match(source, R"(\b(?:broker|BROKER)\s*=\s*['\"]([^'\"]+)['\"])", m)) broker = m[1];
    if (match(source, R"(\b(?:topic|TOPIC)\s*[:=]\s*['\"]([^'\"]+)['\"])", m)) topic = m[1];
    if (!topic.empty()) {
        const auto send = source.find("producer.send");
        if (send != std::string::npos && source.find(topic, send) != std::string::npos)
            emit(out, repo, file, function_near(source, send, "topic_reference"), "topic",
                 "provider", broker.empty() ? "" : "kafka|" + lower(broker) + "|" + topic,
                 "observed");
        if (match(source, R"(\bsubscribe\s*\(\s*(?:TOPIC|topic)\s*,\s*([A-Za-z_][A-Za-z0-9_]*))",
                  m) ||
            match(source,
                  R"(\bbrokerSubscribe\s*\(\s*broker\s*,\s*topic\s*,\s*([A-Za-z_][A-Za-z0-9_]*))",
                  m))
            emit(out, repo, file, m[1], "topic", "consumer",
                 broker.empty() ? "" : "kafka|" + lower(broker) + "|" + topic, "observed");
    }
    if (source.find("Subscribe(topic)") != std::string::npos)
        emit(out, repo, file, "Subscribe", "topic", "consumer", "", "observed", "dynamic topic");
}
} // namespace

std::vector<ContractLink>
resolve_contract_links(const std::vector<ContractEvidence>& all_evidence) {
    std::vector<ContractLink> links;
    std::unordered_map<std::string, std::vector<std::string>> rpc_declarations;
    std::unordered_map<std::string, std::vector<std::string>> operation_by_http;
    std::unordered_map<std::string, int> operation_occurrences;
    for (const auto& e : all_evidence) {
        if (e.surface == "grpc" && e.role == "declaration" && !e.identity.empty()) {
            const auto first = e.identity.find('|');
            if (first != std::string::npos)
                rpc_declarations[e.identity.substr(first + 1)].push_back(e.identity);
        }
        if (e.surface == "http" && e.role == "declaration" &&
            e.detail.rfind("operationId=", 0) == 0)
            operation_by_http[e.identity].push_back(e.repository + "|" + e.detail.substr(12));
        if (e.surface == "openapi" && e.role == "declaration") ++operation_occurrences[e.identity];
    }
    auto qualified = [&](const ContractEvidence& e) {
        if (e.identity.empty()) return std::string{};
        if (e.surface != "grpc" || e.identity.find('|') != e.identity.rfind('|')) return e.identity;
        auto it = rpc_declarations.find(e.identity);
        return it != rpc_declarations.end() && it->second.size() == 1 ? it->second.front()
                                                                      : std::string{};
    };
    for (const auto& provider : all_evidence) {
        const bool declared_sender = provider.surface == "topic" &&
                                     provider.role == "declaration" &&
                                     provider.detail == "provider";
        if (provider.role != "provider" && provider.surface != "openapi" && !declared_sender)
            continue;
        std::string provider_key = qualified(provider);
        if (provider_key.empty()) continue;
        for (const auto& consumer : all_evidence) {
            const bool declared_receiver = consumer.surface == "topic" &&
                                           consumer.role == "declaration" &&
                                           consumer.detail == "consumer";
            if ((consumer.role != "consumer" && !declared_receiver) ||
                consumer.repository == provider.repository)
                continue;
            const std::string consumer_key = qualified(consumer);
            if (consumer_key.empty()) continue;
            bool found = provider.surface == consumer.surface && provider_key == consumer_key;
            if (provider.surface == "openapi" && consumer.surface == "http") {
                const auto it = operation_by_http.find(consumer_key);
                found = it != operation_by_http.end() && it->second.size() == 1 &&
                        it->second.front() == provider_key &&
                        operation_occurrences[provider_key] == 1;
            }
            if (found)
                links.push_back(
                    {provider, consumer, provider_key,
                     consumer_key == consumer.identity ? "exact" : "declaration_resolved"});
        }
    }
    return links;
}

std::vector<ContractEvidence> extract_contracts(const fs::path& root, const std::string& repo) {
    std::vector<ContractEvidence> out;
    if (!fs::exists(root)) return out;
    std::unordered_map<std::string, std::vector<std::string>> proto_methods;
    auto skipped = [](const fs::path& path) {
        const auto name = path.filename().string();
        return (!name.empty() && name.front() == '.') || name == "CMakeFiles" ||
               name == "node_modules" || name == "build" || name == "dist" || name == "target" ||
               name == ".venv" || name == "vendor" || name == ".worktrees";
    };
    // Contracts first; source evidence may refer to their qualified identities.
    for (auto it =
             fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied);
         it != fs::end(it); ++it) {
        if (it->is_directory() && skipped(it->path())) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        const std::string ext = lower(it->path().extension().string());
        if (ext != ".yaml" && ext != ".yml" && ext != ".proto") continue;
        const std::string file = fs::relative(it->path(), root).generic_string();
        const std::string source = read_text(it->path());
        if (ext == ".proto")
            extract_proto(source, repo, file, out);
        else {
            extract_openapi(source, repo, file, out);
            extract_asyncapi(source, repo, file, out);
        }
    }
    for (const auto& e : out)
        if (e.surface == "grpc" && e.role == "declaration") {
            auto pos = e.identity.find_last_of('|');
            proto_methods[e.identity.substr(pos + 1)].push_back(e.identity);
        }
    for (auto it =
             fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied);
         it != fs::end(it); ++it) {
        if (it->is_directory() && skipped(it->path())) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        const std::string ext = lower(it->path().extension().string());
        if (ext != ".ts" && ext != ".js" && ext != ".py" && ext != ".java" && ext != ".go" &&
            ext != ".cs")
            continue;
        extract_source(read_text(it->path()), repo, fs::relative(it->path(), root).generic_string(),
                       out, proto_methods);
    }
    return out;
}

void ensure_contract_schema(duckdb::Connection& conn) {
    require_success(
        conn.Query(
            "CREATE TABLE IF NOT EXISTS contract_evidence ("
            "repository VARCHAR NOT NULL, file_path VARCHAR NOT NULL, symbol VARCHAR NOT NULL,"
            "surface VARCHAR NOT NULL, role VARCHAR NOT NULL, identity VARCHAR NOT NULL,"
            "origin VARCHAR NOT NULL, ambiguity VARCHAR NOT NULL, detail VARCHAR NOT NULL)"),
        "create contract evidence");
}

bool replace_contract_evidence(duckdb::Connection& conn,
                               const std::vector<ContractEvidence>& evidence) {
    ensure_contract_schema(conn);
    using Row = std::array<std::string, 9>;
    auto row_of = [](const ContractEvidence& e) -> Row {
        return {e.repository, e.file,   e.symbol,    e.surface, e.role,
                e.identity,   e.origin, e.ambiguity, e.detail};
    };
    std::vector<Row> desired, stored;
    desired.reserve(evidence.size());
    for (const auto& e : evidence)
        desired.push_back(row_of(e));
    auto existing = conn.Query("SELECT repository,file_path,symbol,surface,role,identity,"
                               "origin,ambiguity,detail FROM contract_evidence");
    require_ok(existing, "load contract evidence");
    stored.reserve(existing->RowCount());
    for (duckdb::idx_t i = 0; i < existing->RowCount(); ++i) {
        Row row;
        for (int col = 0; col < 9; ++col)
            row[col] = existing->GetValue(col, i).ToString();
        stored.push_back(std::move(row));
    }
    std::sort(desired.begin(), desired.end());
    std::sort(stored.begin(), stored.end());
    if (desired == stored) return false;
    require_success(conn.Query("DELETE FROM contract_evidence"), "replace contract evidence");
    auto stmt = conn.Prepare("INSERT INTO contract_evidence VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (stmt->HasError()) throw std::runtime_error(stmt->GetError());
    for (const auto& e : evidence) {
        duckdb::vector<duckdb::Value> values = {e.repository, e.file,      e.symbol,
                                                e.surface,    e.role,      e.identity,
                                                e.origin,     e.ambiguity, e.detail};
        auto result = stmt->Execute(values);
        if (result->HasError()) throw std::runtime_error(result->GetError());
    }
    return true;
}

} // namespace axon
