#include "impact_queries.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace axon {

std::vector<ApiFieldFinding> compare_response_fields(const ResponseShape& shape) {
    std::map<std::string, std::string> guaranteed;
    for (const auto& field : shape.guaranteed_fields)
        if (!field.path.empty()) guaranteed[field.path] = field.origin;
    std::set<std::string> observed;
    for (const auto& field : shape.observed_fields)
        if (!field.path.empty()) observed.insert(field.path);

    std::vector<ApiFieldFinding> findings;
    for (const auto& access : shape.accesses) {
        ApiFieldFinding finding;
        finding.link_identity = shape.link_identity;
        finding.provider_symbol = shape.provider_symbol;
        finding.consumer_symbol = shape.consumer_symbol;
        finding.field = access.path;
        finding.consumer_origin = access.origin;
        const auto present = guaranteed.find(access.path);
        if (shape.link_identity.empty()) {
            finding.verdict = "unknown";
            finding.reason = "consumer and provider have no confirmed contract link";
        } else if (access.dynamic || access.path.empty()) {
            finding.verdict = "unknown";
            finding.reason = "consumer field access is dynamic";
        } else if (present != guaranteed.end()) {
            finding.verdict = "compatible";
            finding.producer_origin = present->second;
            finding.reason = "accessed field is guaranteed by the response";
        } else if (observed.count(access.path)) {
            finding.verdict = "possible";
            finding.producer_origin = "observed";
            finding.reason = "field occurs in only some response branches";
        } else if (shape.closed && access.required &&
                   (shape.closure_origin == "observed" || shape.closure_origin == "declared")) {
            finding.verdict = "proven";
            finding.producer_origin = shape.closure_origin;
            finding.reason = "required access is absent from the complete response shape";
        } else if (!guaranteed.empty() || shape.closed) {
            finding.verdict = "possible";
            finding.producer_origin = shape.closure_origin;
            finding.reason =
                shape.closed ? "access is optional or guarded" : "response shape is incomplete";
        } else {
            finding.verdict = "unknown";
            finding.reason = "no response field evidence is available";
        }
        findings.push_back(std::move(finding));
    }
    std::sort(findings.begin(), findings.end(), [](const auto& a, const auto& b) {
        return std::tie(a.field, a.consumer_origin, a.verdict) <
               std::tie(b.field, b.consumer_origin, b.verdict);
    });
    return findings;
}

std::vector<SymbolCommunity>
group_symbol_communities(const std::vector<std::string>& symbols,
                         const std::vector<SymbolRelation>& relations) {
    std::set<std::string> ids(symbols.begin(), symbols.end());
    ids.erase("");
    std::vector<std::pair<std::string, std::string>> links;
    for (const auto& edge : relations) {
        if (!edge.resolved || (edge.kind != "calls" && edge.kind != "imports") ||
            edge.from.empty() || edge.to.empty())
            continue;
        ids.insert(edge.from);
        ids.insert(edge.to);
        auto pair = std::minmax(edge.from, edge.to);
        links.emplace_back(pair.first, pair.second);
    }
    std::vector<std::string> ordered(ids.begin(), ids.end());
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < ordered.size(); ++i)
        index[ordered[i]] = i;
    std::vector<std::size_t> parent(ordered.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<std::size_t(std::size_t)> root = [&](std::size_t x) -> std::size_t {
        return parent[x] == x ? x : parent[x] = root(parent[x]);
    };
    std::sort(links.begin(), links.end());
    links.erase(std::unique(links.begin(), links.end()), links.end());
    for (const auto& [a, b] : links) {
        auto ra = root(index[a]);
        auto rb = root(index[b]);
        if (ra != rb) parent[std::max(ra, rb)] = std::min(ra, rb);
    }
    std::map<std::size_t, SymbolCommunity> grouped;
    for (std::size_t i = 0; i < ordered.size(); ++i)
        grouped[root(i)].symbols.push_back(ordered[i]);
    for (const auto& [a, b] : links)
        ++grouped[root(index[a])].relation_count;
    std::vector<SymbolCommunity> result;
    for (auto& [_, community] : grouped) {
        community.id = community.symbols.front();
        result.push_back(std::move(community));
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    return result;
}

ExecutionTrace trace_execution_paths(const std::vector<ExecutionNode>& nodes,
                                     const std::vector<ExecutionEdge>& edges,
                                     const TraceLimits& limits) {
    ExecutionTrace result;
    std::map<std::string, ExecutionNode> by_id;
    for (const auto& node : nodes)
        if (!node.id.empty()) by_id[node.id] = node;
    std::map<std::string, std::vector<ExecutionEdge>> outgoing;
    for (const auto& edge : edges) {
        if (by_id.count(edge.from) && by_id.count(edge.to)) outgoing[edge.from].push_back(edge);
    }
    for (auto& [_, next] : outgoing) {
        std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
            return std::tie(a.to, a.kind, a.evidence) < std::tie(b.to, b.kind, b.evidence);
        });
        next.erase(std::unique(next.begin(), next.end(),
                               [](const auto& a, const auto& b) {
                                   return std::tie(a.to, a.kind, a.evidence) ==
                                          std::tie(b.to, b.kind, b.evidence);
                               }),
                   next.end());
    }
    std::vector<std::string> path;
    std::vector<std::string> kinds;
    std::vector<std::string> evidence;
    std::vector<bool> uncertain;
    std::unordered_set<std::string> on_path;
    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        if (result.paths.size() >= limits.max_paths) {
            result.truncated = true;
            return;
        }
        const auto& node = by_id.at(id);
        if (node.kind == "output") {
            result.paths.push_back({path, kinds, evidence, uncertain, node.output_kind});
            return;
        }
        const auto found = outgoing.find(id);
        if (found == outgoing.end()) return;
        const auto& next = found->second;
        if (kinds.size() >= limits.max_depth) {
            result.truncated = true;
            return;
        }
        if (next.size() > limits.max_branching) result.truncated = true;
        for (std::size_t i = 0; i < std::min(next.size(), limits.max_branching); ++i) {
            const auto& edge = next[i];
            if (on_path.count(edge.to)) continue;
            on_path.insert(edge.to);
            path.push_back(edge.to);
            kinds.push_back(edge.kind);
            evidence.push_back(edge.evidence);
            uncertain.push_back(edge.evidence != "observed");
            visit(edge.to);
            uncertain.pop_back();
            evidence.pop_back();
            kinds.pop_back();
            path.pop_back();
            on_path.erase(edge.to);
            if (result.paths.size() >= limits.max_paths) {
                if (i + 1 < next.size()) result.truncated = true;
                break;
            }
        }
    };
    for (const auto& [id, node] : by_id) {
        if (node.kind != "http_handler" && node.kind != "rpc_handler" &&
            node.kind != "event_handler")
            continue;
        if (result.paths.size() >= limits.max_paths) {
            result.truncated = true;
            break;
        }
        path = {id};
        on_path = {id};
        visit(id);
    }
    return result;
}

} // namespace axon
