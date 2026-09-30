#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace axon {

// These inputs are hydrated by the query boundary from indexed symbols and
// confirmed contract links. Empty link_identity means no confirmed link.
struct ResponseField {
    std::string path;
    std::string origin; // observed or declared
};

struct ConsumerFieldAccess {
    std::string path;
    std::string origin; // observed or declared
    bool required = true; // false when guarded by an optional/null check
    bool dynamic = false;
};

struct ResponseShape {
    std::string link_identity;
    std::string provider_symbol;
    std::string consumer_symbol;
    std::vector<ResponseField> guaranteed_fields;
    // Fields present in at least one observed response branch. A field outside
    // guaranteed_fields but inside observed_fields is conditional, not absent.
    std::vector<ResponseField> observed_fields;
    std::vector<ConsumerFieldAccess> accesses;
    bool closed = false; // complete, strict response schema or exhaustive observed shape
    std::string closure_origin; // observed or declared, when closed
};

struct ApiFieldFinding {
    std::string link_identity;
    std::string provider_symbol;
    std::string consumer_symbol;
    std::string field;
    std::string verdict; // compatible, proven, possible, unknown
    std::string producer_origin;
    std::string consumer_origin;
    std::string reason;
};

std::vector<ApiFieldFinding> compare_response_fields(const ResponseShape& shape);

struct SymbolRelation {
    std::string from;
    std::string to;
    std::string kind; // calls or imports
    bool resolved = true;
};

struct SymbolCommunity {
    std::string id; // lexicographically smallest member; stable across input order
    std::vector<std::string> symbols;
    std::size_t relation_count = 0;
};

// Weakly connected components of confirmed call/import relations. Isolated
// symbols remain visible. Unresolved relations never join communities.
std::vector<SymbolCommunity> group_symbol_communities(
    const std::vector<std::string>& symbols,
    const std::vector<SymbolRelation>& relations);

struct ExecutionNode {
    std::string id;
    std::string kind; // http_handler, rpc_handler, event_handler, call, output
    std::string output_kind; // return, persistence, external_http, publish, etc.
};

struct ExecutionEdge {
    std::string from;
    std::string to;
    std::string kind; // calls, imports, invokes, emits
    std::string evidence; // observed, declared, inferred
};

struct ExecutionPath {
    std::vector<std::string> nodes; // entry first, output last
    std::vector<std::string> edge_kinds;
    std::vector<std::string> edge_evidence;
    std::vector<bool> uncertain_edges;
    std::string output_kind;
};

struct ExecutionTrace {
    std::vector<ExecutionPath> paths;
    bool truncated = false;
};

struct TraceLimits {
    std::size_t max_depth = 8; // number of edges
    std::size_t max_branching = 8;
    std::size_t max_paths = 50;
};

// Traverses only supplied edges, with deterministic ordering and cycle guard.
// Unknown/dynamic edges have no destination and must not be supplied as links.
ExecutionTrace trace_execution_paths(const std::vector<ExecutionNode>& nodes,
                                    const std::vector<ExecutionEdge>& edges,
                                    const TraceLimits& limits = {});

} // namespace axon
