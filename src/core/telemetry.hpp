#pragma once

#include "config.hpp"
#include "db.hpp"
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace axon {

struct TelemetryEvent {
    std::string type;
    std::string origin;
    int64_t latency_ms = 0;
    int64_t tokens_estimated = 0;
    int64_t baseline_tokens_estimated = 0;
    int64_t tokens_saved = 0;
    bool cache_hit = false;
};

bool telemetry_enabled(const Config& cfg);
double cost_per_m_input_usd();
void record_telemetry(const Config& cfg, Database* db, const TelemetryEvent& event);
nlohmann::json metrics_json(const Config& cfg, Database* db);

} // namespace axon
