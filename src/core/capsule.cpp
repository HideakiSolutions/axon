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

static std::vector<int64_t> select_pivots_by_query(
    const std::string& query_text,
    Database& db,
    EmbeddingModel& model,
    int top_k = 5)
{
    auto qvec = model.embed(query_text);

    // Build FLOAT array literal for DuckDB
    std::ostringstream vec_str;
    vec_str << "[";
    for (size_t i = 0; i < qvec.size(); i++) {
        if (i) vec_str << ",";
        vec_str << qvec[i];
    }
    vec_str << "]";

    std::string sql =
        "SELECT DISTINCT file_id "
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

    std::vector<int64_t> ids;
    std::unordered_set<int64_t> seen;
    for (duckdb::idx_t i = 0; i < mat.RowCount() && (int)ids.size() < top_k; i++) {
        int64_t fid = mat.GetValue<int64_t>(0, i);
        if (!seen.count(fid)) { seen.insert(fid); ids.push_back(fid); }
    }
    return ids;
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
    // 1. Select pivots
    std::vector<int64_t> pivot_ids;
    if (!explicit_pivots.empty()) {
        for (const auto& p : explicit_pivots) {
            auto it = graph.path_to_id.find(p);
            if (it != graph.path_to_id.end()) pivot_ids.push_back(it->second);
        }
    }
    if (pivot_ids.empty() && !query.empty()) {
        pivot_ids = select_pivots_by_query(query, db, model);
    }
    if (pivot_ids.empty()) {
        std::cerr << "[axon] No pivots found. Run `axon index` first.\n";
        return {};
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
