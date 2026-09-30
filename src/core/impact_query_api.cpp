#include "impact_query_api.hpp"
#include "impact_query_adapter.hpp"
#include "data_trace.hpp"
#include "registry.hpp"
#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>

namespace axon {
namespace {
using json = nlohmann::json;

std::vector<ContractEvidence> read_evidence(duckdb::Connection& conn) {
    std::vector<ContractEvidence> out;
    auto rows = conn.Query("SELECT repository,file_path,symbol,surface,role,identity,"
                           "origin,ambiguity,detail FROM contract_evidence");
    if (rows->HasError()) return out;
    for (duckdb::idx_t i = 0; i < rows->RowCount(); ++i)
        out.push_back({rows->GetValue(0, i).ToString(), rows->GetValue(1, i).ToString(),
                       rows->GetValue(2, i).ToString(), rows->GetValue(3, i).ToString(),
                       rows->GetValue(4, i).ToString(), rows->GetValue(5, i).ToString(),
                       rows->GetValue(6, i).ToString(), rows->GetValue(7, i).ToString(),
                       rows->GetValue(8, i).ToString()});
    return out;
}

json trace_json(const DataTraceResult& result) {
    json paths = json::array();
    for (const auto& p : result.paths) {
        json steps = json::array();
        for (const auto& s : p.steps)
            steps.push_back({{"kind", s.kind},
                             {"expression", s.expression},
                             {"line", s.line},
                             {"evidence", s.evidence}});
        paths.push_back({{"function", p.function},
                         {"source", p.source},
                         {"sink", p.sink},
                         {"sink_kind", p.sink_kind},
                         {"status", p.status},
                         {"reason", p.reason},
                         {"steps", steps}});
    }
    return {{"file", result.file},
            {"supported", result.supported},
            {"paths", paths},
            {"functions_scanned", result.functions_scanned},
            {"sinks_seen", result.sinks_seen},
            {"unknown_sinks", result.unknown_sinks},
            {"source_bytes", result.source_bytes},
            {"trace_bytes", result.trace_bytes},
            {"elapsed_us", result.elapsed_us},
            {"persistent_index_bytes", 0},
            {"limitations", result.limitations},
            {"experimental", true}};
}
} // namespace

json run_impact_query(const std::string& name, const json& args, Database& db,
                      const std::filesystem::path& root) {
    if (name == "trace_data_flow") {
        const std::string file = args.value("file", "");
        if (file.empty()) return {{"error", "file is required"}};
        const auto relative = std::filesystem::path(file).lexically_normal();
        if (relative.is_absolute() || relative.empty() ||
            std::find(relative.begin(), relative.end(), std::filesystem::path("..")) !=
                relative.end())
            return {{"error", "file must be a project-relative path"}};
        std::error_code ec;
        const auto canonical_root = std::filesystem::weakly_canonical(root, ec);
        if (ec) return {{"error", "project root is unavailable"}};
        const auto canonical_file = std::filesystem::weakly_canonical(root / relative, ec);
        if (ec || (canonical_file != canonical_root &&
                   std::mismatch(canonical_root.begin(), canonical_root.end(),
                                 canonical_file.begin(), canonical_file.end())
                           .first != canonical_root.end()))
            return {{"error", "file resolves outside the project"}};
        std::optional<std::string> symbol;
        if (args.contains("symbol") && args["symbol"].is_string())
            symbol = args["symbol"].get<std::string>();
        return trace_json(trace_data_flow(canonical_file, symbol));
    }

    const std::string repo = root.filename().string();
    auto local = load_impact_index(db.conn(), root, repo);
    if (name == "symbol_communities") {
        std::vector<std::string> ids;
        for (const auto& s : local.symbols)
            ids.push_back(s.id);
        json communities = json::array();
        for (const auto& c : group_symbol_communities(ids, local.relations))
            communities.push_back(
                {{"id", c.id}, {"symbols", c.symbols}, {"relation_count", c.relation_count}});
        return {{"communities", communities},
                {"truncated", local.truncated},
                {"evidence", "observed_index_edges"}};
    }
    auto evidence = read_evidence(db.conn());
    if (name == "execution_flow") {
        auto graph = hydrate_execution_graph(local, evidence);
        TraceLimits limits;
        if (args.contains("max_depth"))
            limits.max_depth = std::clamp(args.value("max_depth", 8), 1, 16);
        if (args.contains("max_paths"))
            limits.max_paths = std::clamp(args.value("max_paths", 50), 1, 100);
        auto trace = trace_execution_paths(graph.nodes, graph.edges, limits);
        json paths = json::array();
        for (const auto& p : trace.paths)
            paths.push_back({{"nodes", p.nodes},
                             {"edge_kinds", p.edge_kinds},
                             {"edge_evidence", p.edge_evidence},
                             {"uncertain_edges", p.uncertain_edges},
                             {"output_kind", p.output_kind}});
        return {{"paths", paths},
                {"truncated", trace.truncated || local.truncated},
                {"entry_evidence", "resolved_contract"}};
    }
    if (name == "api_shape") {
        std::map<std::string, ImpactIndexSnapshot> indexes;
        indexes.emplace(repo, std::move(local));
        json unavailable = json::array();
        const auto selected = aggregation_repos(load_registry());
        for (const auto& entry : selected.repos) {
            if (entry.root == root.string() || entry.name == repo) continue;
            auto secondary = open_secondary_read_only(entry);
            if (!secondary) {
                unavailable.push_back(
                    {{"repository", entry.name}, {"reason", secondary.error_code}});
                continue;
            }
            duckdb::Connection conn(*secondary.db);
            auto found = read_evidence(conn);
            evidence.insert(evidence.end(), found.begin(), found.end());
            try {
                indexes.emplace(entry.name, load_impact_index(conn, entry.root, entry.name));
            } catch (const std::exception& ex) {
                unavailable.push_back({{"repository", entry.name}, {"reason", ex.what()}});
            }
        }
        json findings = json::array();
        const std::string wanted = args.value("identity", "");
        for (const auto& link : resolve_contract_links(evidence)) {
            if (link.provider.surface != "http" && link.provider.surface != "openapi") continue;
            if (!wanted.empty() && wanted != link.identity) continue;
            auto provider = indexes.find(link.provider.repository);
            auto consumer = indexes.find(link.consumer.repository);
            if (provider == indexes.end() || consumer == indexes.end()) continue;
            const auto shape =
                hydrate_http_response_shape(link, provider->second, consumer->second);
            for (const auto& f : compare_response_fields(shape))
                findings.push_back({{"identity", f.link_identity},
                                    {"provider_symbol", f.provider_symbol},
                                    {"consumer_symbol", f.consumer_symbol},
                                    {"field", f.field},
                                    {"verdict", f.verdict},
                                    {"producer_origin", f.producer_origin},
                                    {"consumer_origin", f.consumer_origin},
                                    {"reason", f.reason},
                                    {"shape_closed", shape.closed}});
        }
        return {{"findings", findings},
                {"unavailable", unavailable},
                {"result_state", findings.empty() ? "no_confirmed_field_finding" : "evaluated"},
                {"scope", "linked_static_http_subset"}};
    }
    return {{"error", "unknown impact query"}};
}
} // namespace axon
