#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace axon {

// Experimental, on-demand TS/JS intrafunction trace. Nothing invokes this from
// indexing; callers must opt in explicitly. A confirmed trace only follows
// straight-line assignments in one function.
struct DataTraceStep {
    std::string kind; // source | assignment | transform | sink
    std::string expression;
    int line = 0;
    std::string evidence; // observed | inferred | unknown
};

struct DataTracePath {
    std::string function;
    std::string source;
    std::string sink;
    std::string sink_kind; // persistence | http
    std::string status;    // confirmed | unknown
    std::vector<DataTraceStep> steps;
    std::string reason;
};

struct DataTraceResult {
    std::string file;
    bool supported = false;
    std::vector<DataTracePath> paths;
    int functions_scanned = 0;
    int sinks_seen = 0;
    int unknown_sinks = 0;
    size_t source_bytes = 0;
    size_t trace_bytes = 0; // approximate in-memory payload, not a disk index
    long long elapsed_us = 0;
    std::vector<std::string> limitations;
};

DataTraceResult trace_data_flow(const std::filesystem::path& file,
                                const std::optional<std::string>& symbol_filter = std::nullopt);

} // namespace axon
