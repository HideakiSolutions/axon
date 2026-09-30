#include "core/contracts.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>

using nlohmann::json;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: contract_eval CORPUS_ROOT LABELS_JSON\n";
        return 2;
    }
    const std::filesystem::path root = argv[1];
    std::ifstream labels_file(argv[2]);
    json labels;
    labels_file >> labels;
    std::vector<axon::ContractEvidence> evidence;
    for (const auto& repo : labels.at("repositories")) {
        auto rows = axon::extract_contracts(root / repo.get<std::string>(), repo.get<std::string>());
        evidence.insert(evidence.end(), rows.begin(), rows.end());
    }
    auto links = axon::resolve_contract_links(evidence);
    if (std::getenv("AXON_CONTRACT_EVAL_DUMP"))
        for (const auto& e : evidence)
            std::cerr << e.repository << '/' << e.file << "::" << e.symbol << ' '
                      << e.surface << ' ' << e.role << ' ' << e.identity << ' '
                      << e.ambiguity << '\n';
    struct Counts { int tp=0, fp=0, fn=0, tn=0, unknown=0; };
    std::map<std::string, Counts> counts;
    std::set<std::string> labeled_positive_links;
    json cases = json::array();
    for (const auto& label : labels.at("labels")) {
        const std::string surface = label.at("surface");
        const std::string producer = label.at("producer");
        const std::string consumer = label.at("consumer");
        const std::string expected = label.at("verdict");
        if (expected == "true")
            labeled_positive_links.insert(surface + "|" + producer + "|" + consumer);
        bool linked = false;
        for (const auto& link : links) {
            if (link.provider.surface != surface ||
                link.provider.repository + "/" + link.provider.file + "::" + link.provider.symbol != producer ||
                link.consumer.repository + "/" + link.consumer.file + "::" + link.consumer.symbol != consumer) continue;
            if (!label.contains("identity") || link.identity == label.at("identity")) linked = true;
        }
        auto& c = counts[surface];
        if (expected == "true") { if (linked) ++c.tp; else ++c.fn; }
        else if (expected == "false") { if (linked) ++c.fp; else ++c.tn; }
        else { if (linked) ++c.fp; else ++c.unknown; }
        cases.push_back({{"surface", surface}, {"producer", producer},
                         {"consumer", consumer}, {"expected", expected},
                         {"linked", linked}});
    }
    json surfaces = json::object();
    bool quality_thresholds_met = true;
    bool sample_adequate = true;
    json unlabeled_links = json::array();
    for (const auto& link : links) {
        const std::string producer = link.provider.repository + "/" + link.provider.file + "::" + link.provider.symbol;
        const std::string consumer = link.consumer.repository + "/" + link.consumer.file + "::" + link.consumer.symbol;
        const std::string key = link.provider.surface + "|" + producer + "|" + consumer;
        if (!labeled_positive_links.count(key))
            unlabeled_links.push_back({{"surface", link.provider.surface},
                                       {"producer", producer}, {"consumer", consumer},
                                       {"identity", link.identity}});
    }
    if (!unlabeled_links.empty()) quality_thresholds_met = false;
    for (const auto& [name, c] : counts) {
        const double precision = c.tp + c.fp ? double(c.tp) / (c.tp + c.fp) : 0;
        const double recall = c.tp + c.fn ? double(c.tp) / (c.tp + c.fn) : 0;
        if (precision < 0.95 || recall < 0.85) quality_thresholds_met = false;
        if (c.tp + c.fn < 5 || c.tn + c.fp < 2) sample_adequate = false;
        surfaces[name] = {{"tp", c.tp}, {"fp", c.fp}, {"fn", c.fn},
                          {"tn", c.tn}, {"unknown", c.unknown},
                          {"precision", precision}, {"recall", recall}};
    }
    const bool gate = quality_thresholds_met && sample_adequate;
    std::cout << json({{"gate_passed", gate},
                       {"quality_thresholds_met", quality_thresholds_met},
                       {"sample_adequate", sample_adequate}, {"surfaces", surfaces},
                       {"cases", cases}, {"evidence_count", evidence.size()},
                       {"link_count", links.size()},
                       {"unlabeled_links", unlabeled_links}}).dump(2) << '\n';
    return gate ? 0 : 1;
}
