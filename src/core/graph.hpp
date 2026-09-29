#pragma once
#include "db.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

namespace axon {

struct GraphNode {
    int64_t file_id;
    std::string path;
    int depth = 0;
    int degree = 0;
};

struct DependencyGraph {
    std::unordered_map<int64_t, std::vector<int64_t>> outgoing;
    std::unordered_map<int64_t, std::vector<int64_t>> incoming;
    std::unordered_map<int64_t, std::string> id_to_path;
    std::unordered_map<std::string, int64_t> path_to_id;
    std::unordered_map<int64_t, int64_t> file_byte_size;
    // Symbol-level adjacency (populated when granularity=symbol edges exist)
    std::unordered_map<int64_t, std::vector<int64_t>> symbol_incoming;

    int degree(int64_t file_id) const;

    // File IDs ordered by (incoming + outgoing) edge count descending. Entry
    // points of the codebase — good for vibe coding / onboarding without a query.
    std::vector<GraphNode> top_files_by_degree(int limit) const;
};

// True for paths matching common test-file conventions across the 13 supported
// languages (*_test.*, test_*, *.spec.*, *.test.*, */tests/*, */__tests__/*).
bool is_test_path(const std::string& path);

DependencyGraph load_graph(Database& db);

struct TraversalResult {
    std::vector<GraphNode> pivot_files;
    std::vector<GraphNode> support_files;
};

TraversalResult bfs_from_pivots(const DependencyGraph& graph, const std::vector<int64_t>& pivot_ids,
                                int max_depth = 2, int token_budget = 8000);

// Symbol-level BFS — expands from pivot symbol IDs via graph.symbol_incoming
// (callers). Returns symbol IDs in BFS order. Hidration of (name, file_id,
// signature, etc.) is the caller's responsibility (do a single SELECT IN).
struct SymbolHit {
    int64_t symbol_id;
    int depth;
};

std::vector<SymbolHit> bfs_symbols_from_pivots(const DependencyGraph& graph,
                                               const std::vector<int64_t>& pivot_symbol_ids,
                                               int max_depth = 1, int max_symbols = 30);

} // namespace axon
