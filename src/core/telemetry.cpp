#include "telemetry.hpp"
#include <cstdlib>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#define close_socket close
#endif

namespace axon {

using json = nlohmann::json;

namespace {

std::string sq(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out;
}

int64_t safe_i64(duckdb::MaterializedQueryResult& res, int col) {
    if (res.RowCount() == 0) return 0;
    auto v = res.GetValue(col, 0);
    if (v.IsNull()) return 0;
    return v.GetValue<int64_t>();
}

double safe_double(duckdb::MaterializedQueryResult& res, int col) {
    if (res.RowCount() == 0) return 0.0;
    auto v = res.GetValue(col, 0);
    if (v.IsNull()) return 0.0;
    return v.GetValue<double>();
}

bool is_known_layer(const std::string& layer) {
    return layer == "retrieval" || layer == "shell_filtering" || layer == "compression" ||
           layer == "cache" || layer == "ccr" || layer == "indexing" || layer == "unknown";
}

std::string infer_layer(const TelemetryEvent& event) {
    if (is_known_layer(event.layer)) return event.layer;
    if (event.cache_hit) return "cache";
    if (event.type.find("compress") != std::string::npos ||
        event.type.find("compression") != std::string::npos) {
        return "compression";
    }
    if (event.origin == "shell" || event.type.find("shell") != std::string::npos ||
        event.type.find("filter") != std::string::npos) {
        return "shell_filtering";
    }
    if (event.type == "index" || event.type == "index-paths" || event.type == "watch") {
        return "indexing";
    }
    if (event.type == "artifact_retrieve" || event.type == "artifact-retrieve" ||
        event.type.find("ccr") != std::string::npos) {
        return "ccr";
    }
    if (event.type == "capsule" || event.type == "get_context_capsule" ||
        event.type == "get_skeleton" || event.type == "get_overview" ||
        event.type == "get_impact_graph" || event.type == "get_callers" ||
        event.type == "get_tests_for" || event.type == "search_memory" ||
        event.type == "turn_search" || event.type == "dialogue_context" ||
        event.type.rfind("/api/capsule", 0) == 0 || event.type.rfind("/api/search", 0) == 0 ||
        event.type.rfind("/api/overview", 0) == 0 || event.type.rfind("/api/graph", 0) == 0) {
        return "retrieval";
    }
    if (event.type.rfind("thread_", 0) == 0 || event.type.rfind("session_", 0) == 0 ||
        event.type.rfind("turn_", 0) == 0 || event.type.rfind("handoff_", 0) == 0 ||
        event.type == "anchor_link") {
        return "dialogue";
    }
    return "unknown";
}

json empty_layer_metrics() {
    json layers = json::object();
    for (const auto* layer :
         {"retrieval", "dialogue", "shell_filtering", "compression", "cache", "ccr", "unknown"}) {
        layers[layer] = {{"requests", 0},
                         {"tokens_sent", 0},
                         {"tokens_saved", 0},
                         {"reduction_percent", 0.0},
                         {"average_latency_ms", 0.0}};
    }
    return layers;
}

void try_post_http(const std::string& endpoint, const std::string& payload) {
    if (endpoint.rfind("http://", 0) != 0) return;
    std::string rest = endpoint.substr(7);
    std::string host = rest;
    std::string path = "/";
    int port = 80;

    auto slash = rest.find('/');
    if (slash != std::string::npos) {
        host = rest.substr(0, slash);
        path = rest.substr(slash);
    }
    auto colon = host.rfind(':');
    if (colon != std::string::npos) {
        try {
            port = std::stoi(host.substr(colon + 1));
        } catch (...) {
            return;
        }
        host = host.substr(0, colon);
    }
    if (host.empty()) return;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
#endif

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* info = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &info) != 0) return;

    int fd = -1;
    for (addrinfo* p = info; p; p = p->ai_next) {
        fd = static_cast<int>(socket(p->ai_family, p->ai_socktype, p->ai_protocol));
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
        close_socket(fd);
        fd = -1;
    }
    freeaddrinfo(info);
    if (fd < 0) return;

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << host << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << payload.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << payload;
    std::string bytes = req.str();
    send(fd, bytes.c_str(), static_cast<int>(bytes.size()), 0);
    close_socket(fd);
#ifdef _WIN32
    WSACleanup();
#endif
}

} // namespace

bool telemetry_enabled(const Config& cfg) {
    return cfg.project_cfg.telemetry;
}

double cost_per_m_input_usd() {
    if (const char* v = std::getenv("AXON_COST_PER_M_INPUT_USD")) {
        try {
            return std::stod(v);
        } catch (...) {
        }
    }
    return 3.0;
}

void record_telemetry(const Config& cfg, Database* db, const TelemetryEvent& event) {
    if (!telemetry_enabled(cfg) || db == nullptr) return;
    try {
        std::string layer = infer_layer(event);
        int64_t baseline = event.baseline_tokens_estimated;
        int64_t saved = event.tokens_saved;
        if (baseline <= 0 && event.tokens_estimated > 0) baseline = event.tokens_estimated;
        if (saved <= 0 && baseline > event.tokens_estimated)
            saved = baseline - event.tokens_estimated;

        db->conn().Query("INSERT INTO telemetry_events "
                         "(id, type, origin, layer, latency_ms, tokens_estimated, "
                         "baseline_tokens_estimated, tokens_saved, cache_hit, created_at) "
                         "VALUES (nextval('seq_id'), '" +
                         sq(event.type) + "', '" + sq(event.origin) + "', '" + sq(layer) + "', " +
                         std::to_string(event.latency_ms) + ", " +
                         std::to_string(event.tokens_estimated) + ", " + std::to_string(baseline) +
                         ", " + std::to_string(saved) + ", " +
                         std::string(event.cache_hit ? "true" : "false") + ", now())");

        if (const char* endpoint = std::getenv("AXON_TELEMETRY_ENDPOINT")) {
            json payload = {{"type", event.type},
                            {"origin", event.origin},
                            {"layer", layer},
                            {"latency_ms", event.latency_ms},
                            {"tokens_estimated", event.tokens_estimated},
                            {"baseline_tokens_estimated", baseline},
                            {"tokens_saved", saved},
                            {"cache_hit", event.cache_hit}};
            try_post_http(endpoint, payload.dump());
        }
    } catch (...) {
    }
}

json metrics_json(const Config& cfg, Database* db) {
    json out = {{"telemetry_enabled", telemetry_enabled(cfg)},
                {"requests", 0},
                {"tokens_sent", 0},
                {"tokens_saved", 0},
                {"reduction_percent", 0.0},
                {"average_latency_ms", 0.0},
                {"cache_hit_rate", 0.0},
                {"estimated_cost_usd", 0.0},
                {"layers", empty_layer_metrics()}};

    if (!db) return out;

    try {
        if (telemetry_enabled(cfg)) {
            auto total =
                db->conn().Query("SELECT COUNT(*), COALESCE(SUM(tokens_estimated),0), "
                                 "COALESCE(SUM(tokens_saved),0), COALESCE(AVG(latency_ms),0), "
                                 "COALESCE(SUM(CASE WHEN cache_hit THEN 1 ELSE 0 END),0) "
                                 "FROM telemetry_events");
            if (!total->HasError() && total->RowCount() > 0) {
                int64_t requests = safe_i64(*total, 0);
                int64_t tokens = safe_i64(*total, 1);
                int64_t saved = safe_i64(*total, 2);
                double avg_latency = safe_double(*total, 3);
                int64_t hits = safe_i64(*total, 4);
                double denom = static_cast<double>(tokens + saved);
                out["requests"] = requests;
                out["tokens_sent"] = tokens;
                out["tokens_saved"] = saved;
                out["reduction_percent"] = denom > 0.0 ? (100.0 * saved / denom) : 0.0;
                out["average_latency_ms"] = avg_latency;
                out["cache_hit_rate"] = requests > 0 ? (static_cast<double>(hits) / requests) : 0.0;
                out["estimated_cost_usd"] = (tokens / 1000000.0) * cost_per_m_input_usd();
            }
            auto by_layer =
                db->conn().Query("SELECT layer, COUNT(*), COALESCE(SUM(tokens_estimated),0), "
                                 "COALESCE(SUM(tokens_saved),0), COALESCE(AVG(latency_ms),0) "
                                 "FROM telemetry_events GROUP BY layer");
            if (!by_layer->HasError()) {
                for (duckdb::idx_t i = 0; i < by_layer->RowCount(); ++i) {
                    std::string layer = by_layer->GetValue(0, i).ToString();
                    if (!is_known_layer(layer)) layer = "unknown";
                    int64_t requests = by_layer->GetValue<int64_t>(1, i);
                    int64_t tokens = by_layer->GetValue<int64_t>(2, i);
                    int64_t saved = by_layer->GetValue<int64_t>(3, i);
                    double avg_latency = by_layer->GetValue<double>(4, i);
                    double denom = static_cast<double>(tokens + saved);
                    out["layers"][layer] = {
                        {"requests", requests},
                        {"tokens_sent", tokens},
                        {"tokens_saved", saved},
                        {"reduction_percent", denom > 0.0 ? (100.0 * saved / denom) : 0.0},
                        {"average_latency_ms", avg_latency}};
                }
            }
        } else {
            auto counts = db->conn().Query("SELECT "
                                           "(SELECT COUNT(*) FROM files), "
                                           "(SELECT COUNT(*) FROM symbols), "
                                           "(SELECT COUNT(*) FROM edges), "
                                           "COALESCE((SELECT SUM(byte_size) FROM files), 0)");
            if (!counts->HasError() && counts->RowCount() > 0) {
                out["graph"] = {{"files", safe_i64(*counts, 0)},
                                {"symbols", safe_i64(*counts, 1)},
                                {"edges", safe_i64(*counts, 2)},
                                {"bytes_indexed", safe_i64(*counts, 3)}};
            }
            auto capsule = db->conn().Query(
                "SELECT payload FROM capsule_cache ORDER BY created_at DESC LIMIT 1");
            if (!capsule->HasError() && capsule->RowCount() > 0) {
                try {
                    auto payload = json::parse(capsule->GetValue(0, 0).ToString());
                    if (payload.contains("token_estimate"))
                        out["last_capsule_token_estimate"] = payload["token_estimate"];
                } catch (...) {
                }
            }
        }
    } catch (...) {
    }

    return out;
}

} // namespace axon
