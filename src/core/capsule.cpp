#include "capsule.hpp"
#include "ccr.hpp"
#include "compress.hpp"
#include "skeleton.hpp"
#include "dialogue.hpp"
#include <blake3.h>
#include <nlohmann/json.hpp>
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
    if (s == "python") return Language::Python;
    if (s == "rust") return Language::Rust;
    if (s == "go") return Language::Go;
    if (s == "csharp") return Language::CSharp;
    if (s == "php") return Language::PHP;
    if (s == "dart") return Language::Dart;
    if (s == "java") return Language::Java;
    if (s == "bash") return Language::Bash;
    if (s == "cpp") return Language::Cpp;
    if (s == "kotlin") return Language::Kotlin;
    if (s == "vue") return Language::Vue;
    if (s == "lua") return Language::Lua;
    if (s == "nix") return Language::Nix;
    if (s == "ruby") return Language::Ruby;
    if (s == "swift") return Language::Swift;
    if (s == "scala") return Language::Scala;
    return std::nullopt;
}

struct PivotMatch {
    int64_t symbol_id;
    int64_t file_id;
};

// Returns top-K matches preserving WHICH symbol matched (not just file).
// File deduplication happens at caller's discretion.
static std::vector<PivotMatch> select_pivots_by_query(const std::string& query_text, Database& db,
                                                      EmbeddingModel& model, int top_k = 5) {
    auto qvec = model.embed(query_text);

    std::ostringstream vec_str;
    vec_str << "[";
    for (size_t i = 0; i < qvec.size(); i++) {
        if (i) vec_str << ",";
        vec_str << qvec[i];
    }
    vec_str << "]";

    std::string sql = "SELECT id, file_id "
                      "FROM symbols "
                      "WHERE embedding IS NOT NULL "
                      "ORDER BY array_cosine_similarity(embedding, " +
                      vec_str.str() + "::FLOAT[" + std::to_string(model.dims()) +
                      "]) DESC "
                      "LIMIT " +
                      std::to_string(top_k * 4);

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
        if (seen_files.count(fid)) continue; // one pivot per file
        seen_files.insert(fid);
        matches.push_back({sid, fid});
    }
    return matches;
}

// Replace invalid UTF-8 bytes with U+FFFD-like marker so nlohmann::json doesn't throw.
static std::string sanitize_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t bytes = 0;
        if (c < 0x80)
            bytes = 1;
        else if ((c & 0xE0) == 0xC0)
            bytes = 2;
        else if ((c & 0xF0) == 0xE0)
            bytes = 3;
        else if ((c & 0xF8) == 0xF0)
            bytes = 4;
        else {
            out += '?';
            i++;
            continue;
        }
        if (i + bytes > s.size()) {
            out += '?';
            break;
        }
        bool ok = true;
        for (size_t j = 1; j < bytes; j++) {
            if ((((unsigned char)s[i + j]) & 0xC0) != 0x80) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            out += '?';
            i++;
            continue;
        }
        out.append(s, i, bytes);
        i += bytes;
    }
    return out;
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
    int64_t id;
    int64_t file_id;
    std::string name;
    std::string kind;
    int start_line;
    int end_line;
    std::string signature;
    std::string docstring;
};

static std::string capsule_expand_command(const std::string& path, bool is_skeleton) {
    std::string quoted_path = nlohmann::json(path).dump();
    if (is_skeleton)
        return "get_context_capsule {\"pivot_files\":[" + quoted_path + "],\"no_cache\":true}";
    return "get_skeleton {\"files\":[" + quoted_path + "]}";
}

static std::string source_ref_for_symbols(const std::string& path,
                                          const std::vector<SymbolRow>& syms) {
    if (syms.empty()) return path;
    int start = syms.front().start_line;
    int end = syms.front().end_line;
    for (const auto& s : syms) {
        start = std::min(start, s.start_line);
        end = std::max(end, s.end_line);
    }
    return path + ":" + std::to_string(start) + "-" + std::to_string(end);
}

static std::vector<SymbolRow> hydrate_symbols(Database& db, const std::vector<int64_t>& ids) {
    std::vector<SymbolRow> out;
    if (ids.empty()) return out;
    std::ostringstream id_list;
    for (size_t i = 0; i < ids.size(); i++) {
        if (i) id_list << ",";
        id_list << ids[i];
    }
    auto res = db.conn().Query("SELECT id, file_id, name, kind, start_line, end_line, "
                               "       COALESCE(signature, ''), COALESCE(docstring, '') "
                               "FROM symbols WHERE id IN (" +
                               id_list.str() + ")");
    if (res->HasError()) return out;
    auto& mat = *res;
    out.reserve(mat.RowCount());
    for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
        SymbolRow r;
        r.id = mat.GetValue<int64_t>(0, i);
        r.file_id = mat.GetValue<int64_t>(1, i);
        r.name = mat.GetValue(2, i).ToString();
        r.kind = mat.GetValue(3, i).ToString();
        r.start_line = mat.GetValue<int32_t>(4, i);
        r.end_line = mat.GetValue<int32_t>(5, i);
        r.signature = mat.GetValue(6, i).ToString();
        r.docstring = mat.GetValue(7, i).ToString();
        out.push_back(std::move(r));
    }
    return out;
}

static ContextCapsule assemble_symbol_mode(const std::string& query,
                                           const std::vector<PivotMatch>& pivots, Database& db,
                                           const DependencyGraph& graph,
                                           const fs::path& project_root, int token_budget,
                                           CapsuleCompression compression) {
    ContextCapsule capsule;
    capsule.query = query;
    capsule.total_files = (int)graph.id_to_path.size();

    // 1. BFS through symbol_incoming starting from pivot symbol IDs
    std::vector<int64_t> pivot_sym_ids;
    pivot_sym_ids.reserve(pivots.size());
    for (const auto& p : pivots)
        pivot_sym_ids.push_back(p.symbol_id);

    // Conservative BFS — small expansion keeps the capsule focused
    auto hits = bfs_symbols_from_pivots(graph, pivot_sym_ids, /*max_depth=*/1, /*max_symbols=*/15);

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
        if (d == 0)
            by_file_pivot[r.file_id].push_back(r);
        else
            by_file_support[r.file_id].push_back(r);
    }

    // Pivot symbols (matched by query) get a generous body cap.
    // Caller symbols (BFS depth>=1) only need signature+head for orientation.
    const int pivot_per_symbol_cap = std::max(150, token_budget / 16);
    const int support_per_symbol_cap = std::max(40, token_budget / 80);

    // Query the file language once per render_file call when Body compression is
    // enabled (avoids N+1 queries inside the per-symbol loop).
    auto query_file_lang = [&](int64_t fid) -> std::optional<Language> {
        auto lr = db.conn().Query("SELECT language FROM files WHERE id = " + std::to_string(fid));
        if (lr->HasError() || lr->RowCount() == 0) return std::nullopt;
        return lang_from_string(lr->GetValue(0, 0).ToString());
    };

    int compression_input_tokens = 0;
    int compression_output_tokens = 0;
    std::vector<std::string> ccr_artifact_ids;

    auto render_file = [&](int64_t fid, const std::vector<SymbolRow>& syms, int file_token_cap,
                           int per_symbol_cap) -> CapsuleFile {
        CapsuleFile cf;
        auto path_it = graph.id_to_path.find(fid);
        if (path_it == graph.id_to_path.end()) return cf;
        cf.path = path_it->second;
        cf.source_ref = source_ref_for_symbols(cf.path, syms);
        cf.expand_command = capsule_expand_command(cf.path, false);

        auto abs = project_root / cf.path;
        std::string content = read_file(abs);

        // Fetch language once per file when Body compression is on.
        std::optional<Language> file_lang;
        if (compression == CapsuleCompression::Body) file_lang = query_file_lang(fid);

        std::ostringstream body;
        body << "// file: " << cf.path << "\n";
        int used = estimate_tokens(body.str());
        for (const auto& s : syms) {
            if (used >= file_token_cap) break;
            std::ostringstream entry;
            entry << "\n// === " << s.name << " (" << s.kind << ") lines " << s.start_line << "-"
                  << s.end_line << " ===\n";
            if (!s.docstring.empty()) entry << "/// " << s.docstring << "\n";
            // Note: signature is intentionally not emitted here — the body slice
            // already starts with the declaration line, avoiding duplication.

            int header_tokens = estimate_tokens(entry.str());
            int body_budget = std::min(per_symbol_cap, file_token_cap - used) - header_tokens;
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
                    // Site 1: compress_body when flag is on; blind substr otherwise.
                    if (compression == CapsuleCompression::Body) {
                        std::string original_slice = slice;
                        int before_tokens = estimate_tokens(slice);
                        std::string compressed = compress_body(slice, file_lang, body_budget);
                        int compressed_tokens = estimate_tokens(compressed);
                        if (compressed_tokens < before_tokens) {
                            std::string source_ref = cf.path + ":" + std::to_string(s.start_line) +
                                                     "-" + std::to_string(s.end_line);
                            std::string artifact_id = ccr_store_artifact(
                                db, "capsule_body", source_ref, original_slice, before_tokens);
                            std::string recoverable =
                                artifact_id.empty()
                                    ? std::string{}
                                    : ccr_marker(artifact_id, before_tokens) + compressed;
                            int recoverable_tokens = estimate_tokens(recoverable);
                            if (!artifact_id.empty() && recoverable_tokens < before_tokens) {
                                slice = std::move(recoverable);
                                compression_output_tokens += recoverable_tokens;
                                ccr_artifact_ids.push_back(std::move(artifact_id));
                                compression_input_tokens += before_tokens;
                            } else {
                                slice = std::move(original_slice);
                            }
                        }
                    } else {
                        int max_chars = body_budget * 4;
                        if ((int)slice.size() > max_chars) slice = slice.substr(0, max_chars);
                        slice += "\n// … (truncated, body too large for budget)\n";
                    }
                }
            }
            body << entry.str() << slice;
            if (!slice.empty() && slice.back() != '\n') body << "\n";
            used = estimate_tokens(body.str());
        }
        cf.content = sanitize_utf8(body.str());
        cf.is_skeleton = false;
        cf.token_estimate = estimate_tokens(cf.content);
        return cf;
    };

    int num_pivots = (int)by_file_pivot.size();
    int pivot_file_cap = (num_pivots > 0) ? (token_budget * 60 / 100) / num_pivots : token_budget;

    int tokens_used = 0;
    for (auto& [fid, syms] : by_file_pivot) {
        if (tokens_used >= token_budget * 90 / 100) break;
        int remaining = token_budget - tokens_used;
        auto cf = render_file(fid, syms, std::min(pivot_file_cap, remaining), pivot_per_symbol_cap);
        if (cf.path.empty()) continue;
        tokens_used += cf.token_estimate;
        capsule.pivot_files.push_back(std::move(cf));
    }
    int support_file_cap =
        std::max(80, (token_budget - tokens_used) / std::max(1, (int)by_file_support.size()));
    for (auto& [fid, syms] : by_file_support) {
        if (tokens_used >= token_budget * 90 / 100) break;
        int remaining = token_budget - tokens_used;
        auto cf =
            render_file(fid, syms, std::min(support_file_cap, remaining), support_per_symbol_cap);
        if (cf.path.empty()) continue;
        tokens_used += cf.token_estimate;
        capsule.support_files.push_back(std::move(cf));
    }
    capsule.token_estimate = tokens_used;
    capsule.compression_input_tokens = compression_input_tokens;
    capsule.compression_output_tokens = compression_output_tokens;
    capsule.compression_tokens_saved =
        std::max(0, compression_input_tokens - compression_output_tokens);
    capsule.ccr_artifact_ids = std::move(ccr_artifact_ids);
    return capsule;
}

ContextCapsule assemble_capsule(const std::string& query,
                                const std::vector<std::string>& explicit_pivots, Database& db,
                                EmbeddingModel& model, const DependencyGraph& graph,
                                const fs::path& project_root, int token_budget, int dialogue_budget,
                                CapsuleCompression compression) {
    // 1. Select pivots — preserves WHICH symbol matched (if query-driven)
    std::vector<PivotMatch> pivot_matches;
    std::vector<int64_t> pivot_ids;
    if (!explicit_pivots.empty()) {
        for (const auto& p : explicit_pivots) {
            auto it = graph.path_to_id.find(p);
            if (it != graph.path_to_id.end()) pivot_ids.push_back(it->second);
        }
    }
    if (pivot_ids.empty() && !query.empty()) {
        pivot_matches = select_pivots_by_query(query, db, model);
        for (const auto& m : pivot_matches)
            pivot_ids.push_back(m.file_id);
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
        auto cap = assemble_symbol_mode(query, pivot_matches, db, graph, project_root, token_budget,
                                        compression);
        // Augment with file-level support if symbol BFS produced little context
        if (cap.token_estimate < token_budget / 4 && graph.symbol_incoming.empty()) {
            // Fall back to file-level BFS for support — pivots stay symbol-rendered
            auto traversal =
                bfs_from_pivots(graph, pivot_ids, 2, token_budget - cap.token_estimate);
            int tokens_used = cap.token_estimate;
            for (const auto& node : traversal.support_files) {
                if (tokens_used >= token_budget) break;
                // Skip files already rendered as pivots
                bool already = false;
                for (const auto& p : cap.pivot_files)
                    if (p.path == node.path) {
                        already = true;
                        break;
                    }
                if (already) continue;

                auto lang_res = db.conn().Query("SELECT language FROM files WHERE id = " +
                                                std::to_string(node.file_id));
                auto& lm = *lang_res;
                if (lm.RowCount() == 0) continue;
                auto lopt = lang_from_string(lm.GetValue(0, 0).ToString());
                auto abs = project_root / node.path;
                auto content = read_file(abs);
                if (content.empty()) continue;
                std::string skel = lopt ? skeletonize(content, *lopt)
                                        : content.substr(0, std::min(content.size(), size_t(300)));
                CapsuleFile cf;
                cf.path = node.path;
                cf.source_ref = node.path;
                cf.expand_command = capsule_expand_command(node.path, true);
                cf.content = skel;
                cf.is_skeleton = true;
                cf.token_estimate = estimate_tokens(skel);
                // A signature-heavy file can skeletonize to more than the whole
                // budget; appending it unchecked is how an 8k-budget capsule
                // reached 11.7k tokens. Skip what doesn't fit and keep trying
                // smaller candidates.
                if (tokens_used + cf.token_estimate > token_budget) continue;
                tokens_used += cf.token_estimate;
                cap.support_files.push_back(std::move(cf));
            }
            cap.token_estimate = tokens_used;
        }
        if (dialogue_budget > 0) {
            auto hits = turns_for_files(db, model, query, pivot_ids, dialogue_budget);
            for (const auto& h : hits) {
                DialogueTurn dt;
                dt.role = h.turn.role;
                dt.content = h.turn.content;
                dt.session_label = h.session_label;
                dt.thread_name = h.thread_name;
                dt.ts = h.turn.ts;
                dt.token_estimate = estimate_tokens(dt.content);
                cap.token_estimate += dt.token_estimate;
                cap.related_turns.push_back(std::move(dt));
            }
        }
        return cap;
    }

    // 2. BFS traversal
    auto traversal = bfs_from_pivots(graph, pivot_ids, 2, token_budget);

    ContextCapsule capsule;
    capsule.query = query;
    capsule.total_files = (int)graph.id_to_path.size();
    int tokens_used = 0;

    // 3. Pivot files — full content if within per-pivot budget, else skeleton
    // Allocate 60% of total budget split equally among pivots
    int num_pivots = (int)traversal.pivot_files.size();
    int pivot_budget_each =
        (num_pivots > 0) ? (token_budget * 60 / 100) / num_pivots : token_budget;

    for (const auto& node : traversal.pivot_files) {
        auto abs = project_root / node.path;
        auto content = read_file(abs);
        if (content.empty()) continue;

        int full_tokens = estimate_tokens(content);
        bool use_full = (full_tokens <= pivot_budget_each);

        std::string body = use_full ? content : [&]() -> std::string {
            auto lang_res = db.conn().Query("SELECT language FROM files WHERE id = " +
                                            std::to_string(node.file_id));
            auto& lm = *lang_res;
            if (lm.RowCount() == 0) return content;
            auto lo = lang_from_string(lm.GetValue(0, 0).ToString());
            return lo ? skeletonize(content, *lo)
                      : content.substr(0, std::min(content.size(), size_t(300)));
        }();

        CapsuleFile cf;
        cf.path = node.path;
        cf.source_ref = node.path;
        cf.expand_command = capsule_expand_command(node.path, !use_full);
        cf.content = body;
        cf.is_skeleton = !use_full;
        cf.token_estimate = estimate_tokens(body);
        tokens_used += cf.token_estimate;
        capsule.pivot_files.push_back(std::move(cf));
    }

    // 4. Support files — skeletonized
    for (const auto& node : traversal.support_files) {
        if (tokens_used >= token_budget) break;

        auto lang_res = db.conn().Query("SELECT language FROM files WHERE id = " +
                                        std::to_string(node.file_id));
        auto& lang_mat = *lang_res;
        if (lang_mat.RowCount() == 0) continue;

        auto lang_opt = lang_from_string(lang_mat.GetValue(0, 0).ToString());
        auto abs = project_root / node.path;
        auto content = read_file(abs);
        if (content.empty()) continue;

        std::string skeleton = lang_opt ? skeletonize(content, *lang_opt)
                                        : content.substr(0, std::min(content.size(), size_t(300)));

        CapsuleFile cf;
        cf.path = node.path;
        cf.source_ref = node.path;
        cf.expand_command = capsule_expand_command(node.path, true);
        cf.content = skeleton;
        cf.is_skeleton = true;
        cf.token_estimate = estimate_tokens(skeleton);
        tokens_used += cf.token_estimate;
        capsule.support_files.push_back(std::move(cf));
    }

    capsule.token_estimate = tokens_used;

    // ── Dialogue integration ────────────────────────────────────────────────
    if (dialogue_budget > 0) {
        auto hits = turns_for_files(db, model, query, pivot_ids, dialogue_budget);
        for (const auto& h : hits) {
            DialogueTurn dt;
            dt.role = h.turn.role;
            dt.content = h.turn.content;
            dt.session_label = h.session_label;
            dt.thread_name = h.thread_name;
            dt.ts = h.turn.ts;
            dt.token_estimate = estimate_tokens(dt.content);
            capsule.token_estimate += dt.token_estimate;
            capsule.related_turns.push_back(std::move(dt));
        }
    }

    return capsule;
}

// ── Cache primitives (W2.T01) ───────────────────────────────────────────────

namespace {

// Escape single quotes for inline SQL strings (parallel to indexer.cpp's sq).
std::string sql_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out;
}

std::string blake3_hex(const std::string& in) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, in.data(), in.size());
    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    char hex[BLAKE3_OUT_LEN * 2 + 1];
    for (size_t i = 0; i < BLAKE3_OUT_LEN; i++)
        snprintf(hex + i * 2, 3, "%02x", out[i]);
    return std::string(hex, BLAKE3_OUT_LEN * 2);
}

nlohmann::json capsule_file_to_json(const CapsuleFile& cf) {
    return nlohmann::json{
        {"path", cf.path},
        {"source_ref", cf.source_ref},
        {"expand_command", cf.expand_command},
        {"content", cf.content},
        {"is_skeleton", cf.is_skeleton},
        {"token_estimate", cf.token_estimate},
    };
}

CapsuleFile capsule_file_from_json(const nlohmann::json& j) {
    CapsuleFile cf;
    cf.path = j.value("path", "");
    cf.source_ref = j.value("source_ref", cf.path);
    cf.content = j.value("content", "");
    cf.is_skeleton = j.value("is_skeleton", true);
    cf.expand_command = j.value("expand_command", capsule_expand_command(cf.path, cf.is_skeleton));
    cf.token_estimate = j.value("token_estimate", 0);
    if (cf.source_ref.empty()) cf.source_ref = cf.path;
    if (cf.expand_command.empty())
        cf.expand_command = capsule_expand_command(cf.path, cf.is_skeleton);
    return cf;
}

} // anonymous namespace

std::string current_project_epoch(Database& db) {
    // CAST to VARCHAR so the string carries microsecond precision regardless
    // of DuckDB's internal TIMESTAMP representation.
    auto res = db.conn().Query("SELECT COALESCE(CAST(MAX(indexed_at) AS VARCHAR), '0') FROM files");
    if (res->HasError() || res->RowCount() == 0) return "0";
    return res->GetValue(0, 0).ToString();
}

std::string compute_capsule_cache_key(const std::string& query, int token_budget,
                                      const std::string& epoch, const std::string& version) {
    // Token budget is part of the key because a 4k-budget capsule and an
    // 8k-budget capsule for the same query render differently. The binary
    // version is part of the key because assembly logic changes across
    // releases — a pre-upgrade entry must not outlive the code that built it
    // (observed: a budget-violating capsule cached by 1.2.8 was still served
    // by 1.2.9 until the index epoch happened to change).
    std::string composite =
        query + "|" + std::to_string(token_budget) + "|" + epoch + "|" + version;
    return blake3_hex(composite);
}

std::optional<ContextCapsule> capsule_cache_lookup(Database& db, const std::string& key,
                                                   const std::string& epoch) {
    auto res = db.conn().Query("SELECT payload FROM capsule_cache "
                               "WHERE query_hash = '" +
                               sql_quote(key) +
                               "' "
                               "AND epoch = '" +
                               sql_quote(epoch) + "' LIMIT 1");
    if (res->HasError() || res->RowCount() == 0) return std::nullopt;

    try {
        std::string payload = res->GetValue(0, 0).ToString();
        auto j = nlohmann::json::parse(payload);
        ContextCapsule cap;
        cap.query = j.value("query", "");
        cap.token_estimate = j.value("token_estimate", 0);
        cap.total_files = j.value("total_files", 0);
        cap.compression_input_tokens = j.value("compression_input_tokens", 0);
        cap.compression_output_tokens = j.value("compression_output_tokens", 0);
        cap.compression_tokens_saved = j.value("compression_tokens_saved", 0);
        for (const auto& id : j.value("ccr_artifact_ids", nlohmann::json::array())) {
            cap.ccr_artifact_ids.push_back(id.get<std::string>());
        }
        for (const auto& pf : j.value("pivot_files", nlohmann::json::array())) {
            cap.pivot_files.push_back(capsule_file_from_json(pf));
        }
        for (const auto& sf : j.value("support_files", nlohmann::json::array())) {
            cap.support_files.push_back(capsule_file_from_json(sf));
        }
        return cap;
    } catch (const std::exception& e) {
        // Malformed cache row — log and treat as miss so the caller falls
        // through to a fresh assemble_capsule().
        std::cerr << "[capsule_cache] payload parse failed: " << e.what() << "\n";
        return std::nullopt;
    }
}

void capsule_cache_insert(Database& db, const std::string& key, const std::string& epoch,
                          const ContextCapsule& capsule) {
    try {
        nlohmann::json j;
        j["query"] = capsule.query;
        j["token_estimate"] = capsule.token_estimate;
        j["total_files"] = capsule.total_files;
        j["compression_input_tokens"] = capsule.compression_input_tokens;
        j["compression_output_tokens"] = capsule.compression_output_tokens;
        j["compression_tokens_saved"] = capsule.compression_tokens_saved;
        j["ccr_artifact_ids"] = capsule.ccr_artifact_ids;
        j["pivot_files"] = nlohmann::json::array();
        j["support_files"] = nlohmann::json::array();
        for (const auto& f : capsule.pivot_files)
            j["pivot_files"].push_back(capsule_file_to_json(f));
        for (const auto& f : capsule.support_files)
            j["support_files"].push_back(capsule_file_to_json(f));
        std::string payload = j.dump();

        // Upsert via DELETE+INSERT — DuckDB supports ON CONFLICT but the
        // explicit two-step is portable across the older versions some users
        // may have vendored.
        db.conn().Query("DELETE FROM capsule_cache WHERE query_hash = '" + sql_quote(key) + "'");
        db.conn().Query(
            "INSERT INTO capsule_cache (query_hash, epoch, payload, created_at) VALUES ('" +
            sql_quote(key) + "', '" + sql_quote(epoch) + "', '" + sql_quote(payload) + "', now())");
    } catch (const std::exception& e) {
        std::cerr << "[capsule_cache] insert failed (non-fatal): " << e.what() << "\n";
    }
}

int capsule_cache_prune(Database& db, const std::string& current_epoch) {
    auto res = db.conn().Query("DELETE FROM capsule_cache WHERE epoch != '" +
                               sql_quote(current_epoch) + "'");
    if (res->HasError()) {
        std::cerr << "[capsule_cache] prune failed (non-fatal): " << res->GetError() << "\n";
        return 0;
    }
    // DuckDB reports a DELETE as a single-row result holding the count.
    if (res->RowCount() == 0) return 0;
    return (int)res->GetValue<int64_t>(0, 0);
}

} // namespace axon
