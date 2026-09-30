#include "core/data_trace.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

// Standalone deterministic evaluator for the experimental, on-demand index.
// Usage: trace_eval <evals/data_trace directory>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::filesystem::path dir = argv[1];
    std::ifstream labels_file(dir / "truth-v1.json");
    if (!labels_file) return 2;
    auto labels = nlohmann::json::parse(labels_file);
    std::map<std::string, axon::DataTraceResult> results;
    for (const std::string file : {"corpus.ts", "corpus.js"})
        results.emplace(file, axon::trace_data_flow(dir / file));
    int tp = 0, fp = 0, eligible = 0, unknown_found = 0, unknown_total = 0;
    int tn = 0, fn = 0;
    for (const auto& label : labels.at("labels")) {
        auto file = label.at("file").get<std::string>();
        auto name = label.at("function").get<std::string>();
        auto expected = label.at("status").get<std::string>();
        std::string actual = "none";
        for (const auto& path : results.at(file).paths)
            if (path.function == name) actual = path.status;
        if (expected == "confirmed") {
            ++eligible;
            if (actual == "confirmed") ++tp;
            else ++fn;
        } else if (expected == "unknown") {
            ++unknown_total;
            if (actual == "unknown") ++unknown_found;
            if (actual == "confirmed") ++fp;
        } else {
            if (actual == "confirmed") ++fp;
            else if (actual == "none") ++tn;
        }
    }
    size_t source_bytes = 0, trace_bytes = 0;
    long long elapsed_us = 0;
    for (const auto& [_, result] : results) {
        source_bytes += result.source_bytes;
        trace_bytes += result.trace_bytes;
        elapsed_us += result.elapsed_us;
    }
    nlohmann::json output = {
        {"precision_confirmed", tp + fp ? nlohmann::json(double(tp) / (tp + fp)) : nlohmann::json(nullptr)},
        {"eligible_coverage", eligible ? nlohmann::json(double(tp) / eligible) : nlohmann::json(nullptr)},
        {"unknown_detection", unknown_total ? nlohmann::json(double(unknown_found) / unknown_total) : nlohmann::json(nullptr)},
        {"true_positive", tp}, {"false_positive", fp}, {"false_negative", fn},
        {"true_negative", tn}, {"eligible_total", eligible},
        {"unknown_found", unknown_found}, {"unknown_total", unknown_total},
        {"source_bytes", source_bytes}, {"trace_bytes", trace_bytes},
        {"elapsed_us", elapsed_us}, {"persistent_index_bytes", 0},
        {"indexing_mode", "on_demand_ephemeral"}
    };
    std::cout << output.dump(2) << '\n';
    return fp || fn || unknown_found != unknown_total ? 1 : 0;
}
