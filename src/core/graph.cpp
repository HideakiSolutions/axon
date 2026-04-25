#include "graph.hpp"
#include <queue>
#include <algorithm>

namespace axon {

int DependencyGraph::degree(int64_t file_id) const {
    int out = 0, inc = 0;
    auto it_out = outgoing.find(file_id);
    if (it_out != outgoing.end()) out = (int)it_out->second.size();
    auto it_inc = incoming.find(file_id);
    if (it_inc != incoming.end()) inc = (int)it_inc->second.size();
    return out + inc;
}

std::vector<GraphNode> DependencyGraph::top_files_by_degree(int limit) const {
    std::vector<GraphNode> nodes;
    nodes.reserve(id_to_path.size());
    for (const auto& [id, path] : id_to_path) {
        GraphNode n;
        n.file_id = id;
        n.path    = path;
        n.degree  = degree(id);
        nodes.push_back(n);
    }
    std::sort(nodes.begin(), nodes.end(),
              [](const GraphNode& a, const GraphNode& b) { return a.degree > b.degree; });
    if ((int)nodes.size() > limit) nodes.resize(limit);
    return nodes;
}

bool is_test_path(const std::string& path) {
    // Common conventions across TS/JS/Python/Rust/Go/C#/PHP/Dart/Java/Bash/C++/Kotlin/Vue.
    // Match on path substring — case-insensitive not needed since these are lowercase conventions.
    auto contains = [&](const char* needle) { return path.find(needle) != std::string::npos; };
    if (contains("/tests/") || contains("/test/") || contains("/__tests__/") ||
        contains("/spec/")  || contains("/testing/")) return true;

    auto has_suffix = [&](const std::string& suf) {
        return path.size() >= suf.size() &&
               path.compare(path.size() - suf.size(), suf.size(), suf) == 0;
    };

    // Filename patterns: _test.X, .test.X, .spec.X, _spec.X
    // Iterate separators because there are many extensions.
    size_t slash = path.find_last_of('/');
    std::string fname = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (fname.rfind("test_", 0) == 0) return true;
    if (fname.find("_test.") != std::string::npos) return true;
    if (fname.find(".test.") != std::string::npos) return true;
    if (fname.find(".spec.") != std::string::npos) return true;
    if (fname.find("_spec.") != std::string::npos) return true;
    if (has_suffix("Test.java") || has_suffix("Tests.java")) return true;
    if (has_suffix("Test.kt")   || has_suffix("Tests.kt"))   return true;
    if (has_suffix("Test.cs")   || has_suffix("Tests.cs"))   return true;
    if (has_suffix("_test.go")) return true;
    return false;
}

DependencyGraph load_graph(Database& db) {
    DependencyGraph g;
    auto& conn = db.conn();

    auto files_res = dq(conn, "SELECT id, path, byte_size FROM files");
    auto& files = require_ok(files_res);
    for (duckdb::idx_t i = 0; i < files.RowCount(); i++) {
        int64_t     id        = files.GetValue<int64_t>(0, i);
        std::string path      = files.GetValue(1, i).ToString();
        int64_t     byte_size = files.GetValue<int64_t>(2, i);
        g.id_to_path[id]      = path;
        g.path_to_id[path]    = id;
        g.file_byte_size[id]  = byte_size;
    }

    auto edges_res = dq(conn, "SELECT from_file, to_file FROM edges");
    auto& edges = require_ok(edges_res);
    for (duckdb::idx_t i = 0; i < edges.RowCount(); i++) {
        int64_t from = edges.GetValue<int64_t>(0, i);
        int64_t to   = edges.GetValue<int64_t>(1, i);
        g.outgoing[from].push_back(to);
        g.incoming[to].push_back(from);
    }

    // Load symbol-level incoming edges when available
    auto sym_res = dq(conn, "SELECT from_symbol, to_symbol FROM edges "
                             "WHERE from_symbol IS NOT NULL AND to_symbol IS NOT NULL");
    if (!sym_res->HasError()) {
        auto& sym_edges = *sym_res;
        for (duckdb::idx_t i = 0; i < sym_edges.RowCount(); i++) {
            int64_t from_sym = sym_edges.GetValue<int64_t>(0, i);
            int64_t to_sym   = sym_edges.GetValue<int64_t>(1, i);
            g.symbol_incoming[to_sym].push_back(from_sym);
        }
    }
    return g;
}

TraversalResult bfs_from_pivots(
    const DependencyGraph& graph,
    const std::vector<int64_t>& pivot_ids,
    int max_depth,
    int token_budget)
{
    // Reserve 40% of budget for support files; convert tokens to bytes (4 bytes/token estimate)
    const int64_t support_byte_limit = (int64_t)(token_budget * 4) * 40 / 100;

    TraversalResult result;
    std::unordered_set<int64_t> visited;
    std::queue<std::pair<int64_t, int>> q;

    for (int64_t pid : pivot_ids) {
        if (visited.count(pid)) continue;
        visited.insert(pid);
        q.push({pid, 0});
    }

    int64_t support_bytes_queued = 0;

    while (!q.empty()) {
        auto [node, depth] = q.front(); q.pop();

        GraphNode gn;
        gn.file_id = node;
        gn.depth   = depth;
        gn.degree  = graph.degree(node);
        auto it = graph.id_to_path.find(node);
        if (it != graph.id_to_path.end()) gn.path = it->second;

        if (depth == 0) {
            result.pivot_files.push_back(gn);
        } else {
            auto sz_it = graph.file_byte_size.find(node);
            int64_t sz = (sz_it != graph.file_byte_size.end()) ? sz_it->second : 0;
            support_bytes_queued += sz;
            result.support_files.push_back(gn);
        }

        // Stop expanding neighbors if support budget already exceeded
        if (depth > 0 && support_bytes_queued >= support_byte_limit) continue;

        if (depth < max_depth) {
            auto enqueue = [&](const std::unordered_map<int64_t, std::vector<int64_t>>& adj) {
                auto it2 = adj.find(node);
                if (it2 == adj.end()) return;
                for (int64_t nb : it2->second)
                    if (!visited.count(nb)) { visited.insert(nb); q.push({nb, depth + 1}); }
            };
            enqueue(graph.outgoing);
            enqueue(graph.incoming);
        }
    }

    std::sort(result.support_files.begin(), result.support_files.end(),
              [](const GraphNode& a, const GraphNode& b) { return a.degree > b.degree; });

    return result;
}

} // namespace axon
