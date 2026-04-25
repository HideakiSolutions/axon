#include "capsule.hpp"
#include "skeleton.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <unordered_set>

namespace axon {
namespace fs = std::filesystem;

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::optional<Language> lang_from_string(const std::string& s) {
    if (s == "typescript") return Language::TypeScript;
    if (s == "javascript") return Language::JavaScript;
    if (s == "python")     return Language::Python;
    if (s == "rust")       return Language::Rust;
    if (s == "go")         return Language::Go;
    if (s == "csharp")     return Language::CSharp;
    if (s == "php")        return Language::PHP;
    if (s == "dart")       return Language::Dart;
    if (s == "java")       return Language::Java;
    return std::nullopt;
}

struct PivotMatch {
    int64_t symbol_id;
    int64_t file_id;
};

// Returns top-K matches preserving WHICH symbol matched (not just file).
// File deduplication happens at caller's discretion.
static std::vector<PivotMatch> select_pivots_by_query(
    const std::string& query_text,
    Database& db,
    EmbeddingModel& model,
    int top_k = 5)
{
    auto qvec = model.embed(query_text);

    std::ostringstream vec_str;
    vec_str << "[";
    for (size_t i = 0; i < qvec.size(); i++) {
        if (i) vec_str << ",";
        vec_str << qvec[i];
    }
    vec_str << "]";

    std::string sql =
        "SELECT id, file_id "
        "FROM symbols "
        "WHERE embedding IS NOT NULL "
        "ORDER BY array_cosine_similarity(embedding, " +
        vec_str.str() + "::FLOAT[" + std::to_string(model.dims()) + "]) DESC "
        "LIMIT " + std::to_string(top_k * 4);

    auto res = dq(db.conn(), sql);
    if (res->HasError()) {
        std::cerr << "[axon] pivot query error: " << res->GetError() << "\n";
        return {};
    }
    auto& mat = require_ok(res);

    std::vector<PivotMatch> matches;
    std::unordered_set<int64_t> seen_files;
    for (duckdb::idx_t i = 0; i < mat.RowCount() && (int)matches.size() < top_k; i++) {
        int64_t sid = mat.GetValue<int64_t>(0, i);
        int64_t fid = mat.GetValue<int64_t>(1, i);
        if (seen_files.count(fid)) continue;  // one pivot per file
        seen_files.insert(fid);
        matches.push_back({sid, fid});
    }
    return matches;
}

// Extract a slice of source between [start_line, end_line] (1-indexed, inclusive).
static std::string extract_lines(const std::string& content, int start_line, int end_line) {
    if (start_line < 1) start_line = 1;
    if (end_line < start_line) end_line = start_line;
    std::string out;
    int line = 1;
    size_t pos = 0;
    while (pos < content.size() && line < start_line) {
        if (content[pos] == '\n') line++;
        pos++;
    }
    size_t start_pos = pos;
    while (pos < content.size() && line <= end_line) {
        if (content[pos] == '\n') line++;
        pos++;
    }
    return content.substr(start_pos, pos - start_pos);
}

struct SymbolRow {
    int64_t     id;
    int64_t     file_id;
    std::string name;
    std::string kind;
    int         start_line;
    int         end_line;
    std::string signature;
    std::string docstring;
};

static std::vector<SymbolRow> hydrate_symbols(Database& db, const std::vector<int64_t>& ids) {
    std::vector<SymbolRow> out;
    if (ids.empty()) return out;
    std::ostringstream id_list;
    for (size_t i = 0; i < ids.size(); i++) {
        if (i) id_list << ",";
        id_list << ids[i];
    }
    auto res = db.conn().Query(
        "SELECT id, file_id, name, kind, start_line, end_line, "
        "       COALESCE(signature, ''), COALESCE(docstring, '') "
        "FROM symbols WHERE id IN (" + id_list.str() + ")");
    if (res->HasError()) return out;
    auto& mat = *res;
    out.reserve(mat.RowCount());
    for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
        SymbolRow r;
        r.id         = mat.GetValue<int64_t>(0, i);
        r.file_id    = mat.GetValue<int64_t>(1, i);
        r.name       = mat.GetValue(2, i).ToString();
        r.kind       = mat.GetValue(3, i).ToString();
        r.start_line = mat.GetValue<int32_t>(4, i);
        r.end_line   = mat.GetValue<int32_t>(5, i);
        r.signature  = mat.GetValue(6, i).ToString();
        r.docstring  = mat.GetValue(7, i).ToString();
        out.push_back(std::move(r));
    }
    return out;
}

static ContextCapsule assemble_symbol_mode(
    const std::string& query,
    const std::vector<PivotMatch>& pivots,
    Database& db,
    const DependencyGraph& graph,
    const fs::path& project_root,
    int token_budget)
{
    ContextCapsule capsule;
    capsule.query       = query;
    capsule.total_files = (int)graph.id_to_path.size();

    // 1. BFS through symbol_incoming starting from pivot symbol IDs
    std::vector<int64_t> pivot_sym_ids;
    pivot_sym_ids.reserve(pivots.size());
    for (const auto& p : pivots) pivot_sym_ids.push_back(p.symbol_id);

    auto hits = bfs_symbols_from_pivots(graph, pivot_sym_ids, /*max_depth=*/1, /*max_symbols=*/30);

    std::vector<int64_t> hit_ids;
    hit_ids.reserve(hits.size());
    std::unordered_map<int64_t, int> depth_of;
    for (const auto& h : hits) {
        hit_ids.push_back(h.symbol_id);
        depth_of[h.symbol_id] = h.depth;
    }

    // 2. Hydrate: pull (file_id, name, kind, lines, signature, docstring)
    auto rows = hydrate_symbols(db, hit_ids);

    // 3. Group by file_id, depth=0 → pivot, depth>=1 → support
    std::unordered_map<int64_t, std::vector<SymbolRow>> by_file_pivot;
    std::unordered_map<int64_t, std::vector<SymbolRow>> by_file_support;
    for (auto& r : rows) {
        int d = depth_of[r.id];
        if (d == 0) by_file_pivot[r.file_id].push_back(r);
        else        by_file_support[r.file_id].push_back(r);
    }

    // Cap per symbol body to keep one giant function from blowing the budget.
    // Each symbol gets at most this many tokens worth of body lines.
    const int per_symbol_cap = std::max(150, token_budget / 12);

    auto render_file = [&](int64_t fid, const std::vector<SymbolRow>& syms,
                           int file_token_cap) -> CapsuleFile {
        CapsuleFile cf;
        auto path_it = graph.id_to_path.find(fid);
        if (path_it == graph.id_to_path.end()) return cf;
        cf.path = path_it->second;

        auto abs = project_root / cf.path;
        std::string content = read_file(abs);

        std::ostringstream body;
        body << "// file: " << cf.path << "\n";
        int used = estimate_tokens(body.str());
        for (const auto& s : syms) {
            if (used >= file_token_cap) break;
            std::ostringstream entry;
            entry << "\n// === " << s.name << " (" << s.kind
                  << ") lines " << s.start_line << "-" << s.end_line << " ===\n";
            if (!s.docstring.empty()) entry << "/// " << s.docstring << "\n";
            // Note: signature is intentionally not emitted here — the body slice
            // already starts with the declaration line, avoiding duplication.

            int header_tokens = estimate_tokens(entry.str());
            int body_budget   = std::min(per_symbol_cap, file_token_cap - used) - header_tokens;
            if (body_budget <= 0) {
                // No room for body — emit signature/header only
                body << entry.str();
                used += header_tokens;
                continue;
            }
            std::string slice;
            if (!content.empty()) {
                slice = extract_lines(content, s.start_line, s.end_line);
                int slice_tokens = estimate_tokens(slice);
                if (slice_tokens > body_budget) {
                    // Truncate body and add ellipsis marker
                    int max_chars = body_budget * 4;
                    if ((int)slice.size() > max_chars) slice = slice.substr(0, max_chars);
                    slice += "\n// … (truncated, body too large for budget)\n";
                }
            }
            body << entry.str() << slice;
            if (!slice.empty() && slice.back() != '\n') body << "\n";
            used = estimate_tokens(body.str());
        }
        cf.content        = body.str();
        cf.is_skeleton    = false;
        cf.token_estimate = estimate_tokens(cf.content);
        return cf;
    };

    int num_pivots = (int)by_file_pivot.size();
    int pivot_file_cap = (num_pivots > 0) ? (token_budget * 60 / 100) / num_pivots : token_budget;

    int tokens_used = 0;
    for (auto& [fid, syms] : by_file_pivot) {
        auto cf = render_file(fid, syms, pivot_file_cap);
        if (cf.path.empty()) continue;
        tokens_used += cf.token_estimate;
        capsule.pivot_files.push_back(std::move(cf));
    }
    int support_file_cap = std::max(100, (token_budget - tokens_used) / std::max(1, (int)by_file_support.size()));
    for (auto& [fid, syms] : by_file_support) {
        if (tokens_used >= token_budget) break;
        auto cf = render_file(fid, syms, support_file_cap);
        if (cf.path.empty()) continue;
        tokens_used += cf.token_estimate;
        capsule.support_files.push_back(std::move(cf));
    }
    capsule.token_estimate = tokens_used;
    return capsule;
}

ContextCapsule assemble_capsule(
    const std::string& query,
    const std::vector<std::string>& explicit_pivots,
    Database& db,
    EmbeddingModel& model,
    const DependencyGraph& graph,
    const fs::path& project_root,
    int token_budget)
{
    // 1. Select pivots — preserves WHICH symbol matched (if query-driven)
    std::vector<PivotMatch> pivot_matches;
    std::vector<int64_t>    pivot_ids;
    if (!explicit_pivots.empty()) {
        for (const auto& p : explicit_pivots) {
            auto it = graph.path_to_id.find(p);
            if (it != graph.path_to_id.end()) pivot_ids.push_back(it->second);
        }
    }
    if (pivot_ids.empty() && !query.empty()) {
        pivot_matches = select_pivots_by_query(query, db, model);
        for (const auto& m : pivot_matches) pivot_ids.push_back(m.file_id);
    }
    if (pivot_ids.empty()) {
        std::cerr << "[axon] No pivots found. Run `axon index` first.\n";
        return {};
    }

    // 1.5. Symbol-mode dispatch — when pivots came from a semantic query
    // (i.e. we know WHICH symbol matched), use granular rendering.
    // BFS via symbol_incoming kicks in only if symbol-level edges are populated;
    // otherwise depth-0 (pivot symbols only) still beats whole-file rendering.
    if (!pivot_matches.empty()) {
        auto cap = assemble_symbol_mode(query, pivot_matches, db, graph, project_root, token_budget);
        // Augment with file-level support if symbol BFS produced little context
        if (cap.token_estimate < token_budget / 4 && graph.symbol_incoming.empty()) {
            // Fall back to file-level BFS for support — pivots stay symbol-rendered
            auto traversal = bfs_from_pivots(graph, pivot_ids, 2, token_budget - cap.token_estimate);
            int tokens_used = cap.token_estimate;
            for (const auto& node : traversal.support_files) {
                if (tokens_used >= token_budget) break;
                // Skip files already rendered as pivots
                bool already = false;
                for (const auto& p : cap.pivot_files) if (p.path == node.path) { already = true; break; }
                if (already) continue;

                auto lang_res = db.conn().Query("SELECT language FROM files WHERE id = " + std::to_string(node.file_id));
                auto& lm = *lang_res;
                if (lm.RowCount() == 0) continue;
                auto lopt = lang_from_string(lm.GetValue(0, 0).ToString());
                auto abs  = project_root / node.path;
                auto content = read_file(abs);
                if (content.empty()) continue;
                std::string skel = lopt ? skeletonize(content, *lopt)
                                        : content.substr(0, std::min(content.size(), size_t(300)));
                CapsuleFile cf;
                cf.path = node.path;
                cf.content = skel;
                cf.is_skeleton = true;
                cf.token_estimate = estimate_tokens(skel);
                tokens_used += cf.token_estimate;
                cap.support_files.push_back(std::move(cf));
            }
            cap.token_estimate = tokens_used;
        }
        return cap;
    }

    // 2. BFS traversal
    auto traversal = bfs_from_pivots(graph, pivot_ids, 2, token_budget);

    ContextCapsule capsule;
    capsule.query       = query;
    capsule.total_files = (int)graph.id_to_path.size();
    int tokens_used     = 0;

    // 3. Pivot files — full content if within per-pivot budget, else skeleton
    // Allocate 60% of total budget split equally among pivots
    int num_pivots = (int)traversal.pivot_files.size();
    int pivot_budget_each = (num_pivots > 0)
        ? (token_budget * 60 / 100) / num_pivots
        : token_budget;

    for (const auto& node : traversal.pivot_files) {
        auto abs     = project_root / node.path;
        auto content = read_file(abs);
        if (content.empty()) continue;

        int full_tokens = estimate_tokens(content);
        bool use_full   = (full_tokens <= pivot_budget_each);

        std::string body = use_full ? content : [&]() -> std::string {
            auto lang_res = db.conn().Query(
                "SELECT language FROM files WHERE id = " + std::to_string(node.file_id));
            auto& lm = *lang_res;
            if (lm.RowCount() == 0) return content;
            auto lo = lang_from_string(lm.GetValue(0, 0).ToString());
            return lo ? skeletonize(content, *lo)
                      : content.substr(0, std::min(content.size(), size_t(300)));
        }();

        CapsuleFile cf;
        cf.path           = node.path;
        cf.content        = body;
        cf.is_skeleton    = !use_full;
        cf.token_estimate = estimate_tokens(body);
        tokens_used      += cf.token_estimate;
        capsule.pivot_files.push_back(std::move(cf));
    }

    // 4. Support files — skeletonized
    for (const auto& node : traversal.support_files) {
        if (tokens_used >= token_budget) break;

        auto lang_res = db.conn().Query("SELECT language FROM files WHERE id = " + std::to_string(node.file_id));
        auto& lang_mat = *lang_res;
        if (lang_mat.RowCount() == 0) continue;

        auto lang_opt = lang_from_string(lang_mat.GetValue(0, 0).ToString());
        auto abs      = project_root / node.path;
        auto content  = read_file(abs);
        if (content.empty()) continue;

        std::string skeleton = lang_opt
            ? skeletonize(content, *lang_opt)
            : content.substr(0, std::min(content.size(), size_t(300)));

        CapsuleFile cf;
        cf.path           = node.path;
        cf.content        = skeleton;
        cf.is_skeleton    = true;
        cf.token_estimate = estimate_tokens(skeleton);
        tokens_used      += cf.token_estimate;
        capsule.support_files.push_back(std::move(cf));
    }

    capsule.token_estimate = tokens_used;
    return capsule;
}

} // namespace axon
