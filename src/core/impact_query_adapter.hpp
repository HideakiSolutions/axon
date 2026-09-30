#pragma once

#include "contracts.hpp"
#include "impact_queries.hpp"

#include <duckdb.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace axon {

struct IndexedImpactSymbol {
    std::string id; // repository-qualified, stable within one index revision
    std::string repository;
    std::string file;
    std::string name;
    std::string body;
};

struct ImpactIndexSnapshot {
    std::vector<IndexedImpactSymbol> symbols;
    std::vector<SymbolRelation> relations;
    bool truncated = false;
};

struct ImpactLoadLimits {
    std::size_t max_symbols = 10000;
    std::size_t max_edges = 30000;
    std::size_t max_body_bytes = 32768;
};

// Read-only hydration; no index writes. Missing/overlarge source bodies remain
// empty, which makes response-shape conclusions unknown instead of guessed.
ImpactIndexSnapshot load_impact_index(duckdb::Connection& conn,
                                      const std::filesystem::path& project_root,
                                      const std::string& repository,
                                      const ImpactLoadLimits& limits = {});

// Strict static subset: literal res.json({...}) in TS/JS handlers and literal
// response.data.field / data.field reads in clients. Closed only if every
// observed response is a parseable object literal and no dynamic return exists.
ResponseShape hydrate_http_response_shape(const ContractLink& link,
                                          const ImpactIndexSnapshot& provider_index,
                                          const ImpactIndexSnapshot& consumer_index);

struct ExecutionGraphInput {
    std::vector<ExecutionNode> nodes;
    std::vector<ExecutionEdge> edges;
};

// Entry points come only from resolved contract evidence. Indexed call targets
// are currently selected by a heuristic resolver, so calls, imports and output
// detection remain inferred until exact call-target provenance is available.
ExecutionGraphInput hydrate_execution_graph(const ImpactIndexSnapshot& index,
                                            const std::vector<ContractEvidence>& evidence);

} // namespace axon
