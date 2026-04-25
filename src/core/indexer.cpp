#include "indexer.hpp"
#include "skeleton.hpp"
#include "../parser/parser.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <sstream>

// Escape single quotes for inline SQL strings
static std::string sq(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) { if (c == '\'') out += '\''; out += c; }
    return out;
}

namespace axon {
namespace fs = std::filesystem;

static const std::vector<std::string> SKIP_DIRS = {
    "node_modules", ".git", "target", "build", "__pycache__",
    ".axon", "dist", ".next", "vendor", ".venv", "venv", ".worktrees"
};

static std::vector<std::string> g_ignore_patterns;

static void load_axonignore(const fs::path& project_root) {
    g_ignore_patterns.clear();
    auto ignore_file = project_root / ".axonignore";
    std::ifstream f(ignore_file);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty()) g_ignore_patterns.push_back(line);
    }
}

static bool should_skip(const fs::path& p) {
    for (const auto& dir : SKIP_DIRS)
        if (p.filename() == dir) return true;
    for (const auto& pat : g_ignore_patterns)
        if (p.filename() == pat) return true;
    return false;
}

// Returns file id, or -1 if unchanged (same hash)
static int64_t upsert_file(duckdb::Connection& conn,
                           const std::string& rel_path,
                           const std::string& lang,
                           const std::string& hash,
                           int64_t byte_size,
                           const std::string& skeleton)
{
    // Check existing
    auto check = conn.Query("SELECT id, hash FROM files WHERE path = '" + sq(rel_path) + "'");
    auto& mat = *check;

    if (mat.RowCount() > 0) {
        int64_t fid = mat.GetValue<int64_t>(0, 0);
        std::string existing_hash = mat.GetValue(1, 0).ToString();
        if (existing_hash == hash) return -1;  // unchanged

        // Updated: delete this file's outgoing edges (will be rebuilt) + symbols.
        // Keep incoming edges — those belong to OTHER files' resolve_edges results
        // and would be lost here since those files aren't being re-walked.
        conn.Query("DELETE FROM edges WHERE from_file = " + std::to_string(fid));
        conn.Query("DELETE FROM symbols WHERE file_id = " + std::to_string(fid));
        conn.Query("UPDATE files SET hash = '" + sq(hash) + "', language = '" + sq(lang) + "', byte_size = " + std::to_string(byte_size) + ", skeleton = '" + sq(skeleton) + "', indexed_at = now() WHERE id = " + std::to_string(fid));
        return fid;
    }

    // New file
    auto res = conn.Query(
        "INSERT INTO files (id, path, language, hash, byte_size, skeleton, indexed_at) "
        "VALUES (nextval('seq_id'), '" + sq(rel_path) + "', '" + sq(lang) + "', '" + sq(hash) + "', " +
        std::to_string(byte_size) + ", '" + sq(skeleton) + "', now()) RETURNING id");
    auto& mat2 = *res;
    return mat2.GetValue<int64_t>(0, 0);
}

static void insert_symbols(duckdb::Connection& conn, int64_t file_id,
                           const std::vector<Symbol>& symbols)
{
    for (const auto& sym : symbols) {
        std::string sql =
            "INSERT INTO symbols (id, file_id, name, kind, start_line, end_line, signature, docstring) "
            "VALUES (nextval('seq_id'), " + std::to_string(file_id) +
            ", '" + sq(sym.name) + "', '" + sq(sym.kind) + "', " +
            std::to_string(sym.start_line) + ", " + std::to_string(sym.end_line) +
            ", '" + sq(sym.signature.value_or("")) + "', '" + sq(sym.docstring.value_or("")) + "')";
        conn.Query(sql);
    }
}

// Resolve an import specifier to a file_id. Tries progressively looser match
// strategies, preferring precise basename matches over substring matches.
//   Input examples:
//     "utils"                    → src/utils.py
//     "app.models.user"          → src/app/models/user.py (or leaf: user.py)
//     "std::collections::HashMap"→ (typically stdlib, no local file)
//     "./helpers"                → src/helpers.ts
//     "@scope/pkg/sub"           → node_modules/@scope/pkg/sub.ts
//     "../lib/x"                 → src/lib/x.py
static int64_t resolve_specifier_to_file(duckdb::Connection& conn,
                                         const std::string& specifier,
                                         int64_t from_id)
{
    auto run = [&](const std::string& sql) -> int64_t {
        auto res = conn.Query(sql);
        if (res->HasError() || res->RowCount() == 0) return 0;
        return res->GetValue<int64_t>(0, 0);
    };

    // try_basename: match paths that end with /<name>.<ext>, <name>.<ext>, or
    // /<name>/index.<ext> — precise enough to avoid "auth" → "test_auth.py".
    auto try_basename = [&](const std::string& name) -> int64_t {
        if (name.size() < 2) return 0;
        std::string n = sq(name);
        std::string sql =
            "SELECT id FROM files WHERE id != " + std::to_string(from_id) + " AND ("
            "  path LIKE '%/" + n + ".%'"
            " OR path LIKE '" + n + ".%'"
            " OR path LIKE '%/" + n + "/index.%'"
            " OR path LIKE '%/" + n + "/mod.%'"   // Rust module file convention
            " OR path LIKE '%/" + n + ".d.ts'"    // TS declaration
            ") LIMIT 1";
        return run(sql);
    };

    // try_contains: last-resort substring match (loose, may overmatch).
    auto try_contains = [&](const std::string& needle) -> int64_t {
        if (needle.size() < 3) return 0;
        return run("SELECT id FROM files WHERE path LIKE '%" + sq(needle) + "%' "
                   "AND id != " + std::to_string(from_id) + " LIMIT 1");
    };

    // Normalize: strip leading "./", "../" chain, and surrounding whitespace.
    std::string s = specifier;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
    while (s.size() >= 2 && s[0] == '.' && (s[1] == '/' || s[1] == '.')) {
        size_t cut = s.find('/');
        if (cut == std::string::npos) break;
        s = s.substr(cut + 1);
    }
    if (s.empty()) return 0;

    // 1) If specifier contains '/', prefer suffix path match (likely already a relative path).
    if (s.find('/') != std::string::npos) {
        int64_t id = run("SELECT id FROM files WHERE path LIKE '%/" + sq(s) + ".%' "
                         "AND id != " + std::to_string(from_id) + " LIMIT 1");
        if (!id) id = run("SELECT id FROM files WHERE path LIKE '%" + sq(s) + ".%' "
                          "AND id != " + std::to_string(from_id) + " LIMIT 1");
        if (id) return id;
    }

    // 2) Tokenize on separators (., /, \, ::)
    std::vector<std::string> parts;
    {
        std::string cur;
        for (size_t i = 0; i < s.size(); ) {
            char c = s[i];
            if (c == ':' && i + 1 < s.size() && s[i + 1] == ':') {
                if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
                i += 2; continue;
            }
            if (c == '.' || c == '/' || c == '\\') {
                if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
                i++; continue;
            }
            cur += c; i++;
        }
        if (!cur.empty()) parts.push_back(cur);
    }

    // 3) Try basename match leaf-first (most specific import target)
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        int64_t id = try_basename(*it);
        if (id) return id;
    }

    // 4) Full specifier as basename (in case it's a single-token module)
    int64_t id = try_basename(s);
    if (id) return id;

    // 5) Last resort: substring match on the leaf token (may be imprecise)
    if (!parts.empty()) {
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
            id = try_contains(*it);
            if (id) return id;
        }
    }
    return 0;
}

static void resolve_edges(duckdb::Connection& conn, int64_t from_id,
                          const std::vector<ImportEdge>& imports,
                          bool symbol_mode)
{
    if (imports.empty()) return;
    for (const auto& edge : imports) {
        int64_t to_id = resolve_specifier_to_file(conn, edge.to_specifier, from_id);
        if (to_id == 0) continue;

        // Insert file-level edge
        auto ins = conn.Query(
            "INSERT INTO edges (id, from_file, to_file, kind) VALUES (nextval('seq_id'), " +
            std::to_string(from_id) + ", " + std::to_string(to_id) +
            ", '" + sq(edge.kind) + "') RETURNING id");

        if (!symbol_mode || ins->HasError() || ins->RowCount() == 0) continue;

        int64_t edge_id = ins->GetValue<int64_t>(0, 0);

        // Extract leaf token from specifier as candidate symbol name
        // e.g. "utils" → "utils", "./auth/parseUser" → "parseUser"
        std::string spec = edge.to_specifier;
        size_t slash = spec.find_last_of("/\\");
        if (slash != std::string::npos) spec = spec.substr(slash + 1);
        // Strip extension if present (e.g. "utils.js" → "utils")
        size_t dot = spec.find_last_of('.');
        if (dot != std::string::npos && dot > 0) spec = spec.substr(0, dot);
        if (spec.empty()) continue;

        // Try to find a symbol in from_file matching the specifier leaf
        auto from_sym = conn.Query(
            "SELECT id FROM symbols WHERE file_id = " + std::to_string(from_id) +
            " AND name = '" + sq(spec) + "' LIMIT 1");
        int64_t from_sym_id = (!from_sym->HasError() && from_sym->RowCount() > 0)
                              ? from_sym->GetValue<int64_t>(0, 0) : 0;

        // Try to find a symbol in to_file with matching name
        auto to_sym = conn.Query(
            "SELECT id FROM symbols WHERE file_id = " + std::to_string(to_id) +
            " AND name = '" + sq(spec) + "' LIMIT 1");
        int64_t to_sym_id = (!to_sym->HasError() && to_sym->RowCount() > 0)
                            ? to_sym->GetValue<int64_t>(0, 0) : 0;

        if (from_sym_id > 0 || to_sym_id > 0) {
            std::string upd = "UPDATE edges SET ";
            if (from_sym_id > 0) upd += "from_symbol = " + std::to_string(from_sym_id);
            if (from_sym_id > 0 && to_sym_id > 0) upd += ", ";
            if (to_sym_id > 0)   upd += "to_symbol = " + std::to_string(to_sym_id);
            upd += " WHERE id = " + std::to_string(edge_id);
            conn.Query(upd);
        }
    }
}

// Returns true if any component of path matches a skip dir or ignore pattern.
static bool path_is_ignored(const fs::path& rel) {
    for (const auto& part : rel) {
        const std::string name = part.filename().string();
        for (const auto& dir : SKIP_DIRS)
            if (name == dir) return true;
        for (const auto& pat : g_ignore_patterns)
            if (name == pat) return true;
    }
    return false;
}

// Remove files from DB whose path no longer exists on disk OR whose path is now ignored.
// Also cleans up dependent symbols and edges. Returns how many files were pruned.
static int sweep_deleted(duckdb::Connection& conn, const fs::path& project_root) {
    auto res = conn.Query("SELECT id, path FROM files");
    if (res->HasError()) return 0;
    auto& mat = *res;

    struct Victim { int64_t id; std::string path; };
    std::vector<Victim> victims;
    for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
        int64_t fid = mat.GetValue<int64_t>(0, i);
        std::string rel = mat.GetValue(1, i).ToString();
        fs::path abs = project_root / rel;
        if (!fs::exists(abs) || path_is_ignored(fs::path(rel)))
            victims.push_back({fid, rel});
    }

    if (victims.empty()) return 0;

    conn.Query("BEGIN TRANSACTION");
    for (const auto& v : victims) {
        conn.Query("DELETE FROM edges WHERE from_file = " + std::to_string(v.id) +
                   " OR to_file = " + std::to_string(v.id));
        conn.Query("DELETE FROM symbols WHERE file_id = " + std::to_string(v.id));
        conn.Query("DELETE FROM files WHERE id = " + std::to_string(v.id));
    }
    conn.Query("COMMIT");
    return (int)victims.size();
}

IndexStats index_project(const Config& cfg, Database& db, ProgressCallback on_progress) {
    IndexStats stats;
    auto& conn = db.conn();

    load_axonignore(cfg.project_root);

    // Collect source files
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(
             cfg.project_root,
             fs::directory_options::skip_permission_denied);
         it != fs::end(it); ++it)
    {
        if (it->is_directory() && should_skip(it->path())) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        auto ext = it->path().extension().string();
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        if (language_from_extension(ext)) files.push_back(it->path());
    }

    int total = (int)files.size();

    // Store parsed results for edge resolution pass
    struct Pending { std::string path; std::vector<ImportEdge> imports; };
    std::vector<Pending> pending_edges;

    conn.Query("BEGIN TRANSACTION");
    for (int i = 0; i < total; i++) {
        const auto& abs_path = files[i];
        if (on_progress) on_progress(abs_path.filename().string(), i, total);

        auto parsed = parse_file(abs_path, cfg.project_root);
        if (!parsed) { stats.files_skipped++; continue; }

        int64_t byte_size = (int64_t)fs::file_size(abs_path);

        // Pre-compute skeleton and store in DB for fast retrieval
        std::string skeleton;
        try {
            std::ifstream sf(abs_path, std::ios::binary);
            if (sf) {
                std::ostringstream ss;
                ss << sf.rdbuf();
                skeleton = skeletonize(ss.str(), parsed->language);
            }
        } catch (...) {}

        int64_t fid = upsert_file(conn, parsed->path,
                                  language_name(parsed->language),
                                  parsed->hash, byte_size, skeleton);
        if (fid == -1) { stats.files_skipped++; continue; }  // unchanged

        insert_symbols(conn, fid, parsed->symbols);
        stats.symbols_found += (int)parsed->symbols.size();
        stats.edges_found   += (int)parsed->imports.size();
        stats.files_indexed++;

        if (!parsed->imports.empty())
            pending_edges.push_back({parsed->path, parsed->imports});
    }
    conn.Query("COMMIT");

    // Resolve edges (second pass — all files now in DB)
    conn.Query("BEGIN TRANSACTION");
    for (const auto& p : pending_edges) {
        auto res = conn.Query("SELECT id FROM files WHERE path = '" + sq(p.path) + "'");
        auto& mat = *res;
        if (mat.RowCount() == 0) continue;
        int64_t fid = mat.GetValue<int64_t>(0, 0);
        resolve_edges(conn, fid, p.imports, cfg.project_cfg.granularity == "symbol");
    }
    conn.Query("COMMIT");

    // Prune deleted and newly-ignored files
    stats.files_pruned = sweep_deleted(conn, cfg.project_root);

    return stats;
}

IndexStats index_files(const Config& cfg, Database& db,
                      const std::vector<fs::path>& paths,
                      bool prune, ProgressCallback on_progress) {
    IndexStats stats;
    auto& conn = db.conn();

    load_axonignore(cfg.project_root);

    // Filter paths: must exist on disk, be regular files, under project_root,
    // and have a supported language extension. Absolute or relative both accepted.
    std::vector<fs::path> abs_paths;
    abs_paths.reserve(paths.size());
    for (const auto& p : paths) {
        fs::path abs = p.is_absolute() ? p : (cfg.project_root / p);
        std::error_code ec;
        abs = fs::weakly_canonical(abs, ec);
        if (ec || !fs::exists(abs) || !fs::is_regular_file(abs)) continue;

        // Must be inside project_root
        auto rel = fs::relative(abs, cfg.project_root, ec);
        if (ec || rel.empty() || rel.native().rfind("..", 0) == 0) continue;

        auto ext = abs.extension().string();
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        if (!language_from_extension(ext)) continue;

        abs_paths.push_back(abs);
    }

    int total = (int)abs_paths.size();

    struct Pending { std::string path; std::vector<ImportEdge> imports; };
    std::vector<Pending> pending_edges;

    if (total > 0) {
        conn.Query("BEGIN TRANSACTION");
        for (int i = 0; i < total; i++) {
            const auto& abs_path = abs_paths[i];
            if (on_progress) on_progress(abs_path.filename().string(), i, total);

            auto parsed = parse_file(abs_path, cfg.project_root);
            if (!parsed) { stats.files_skipped++; continue; }

            int64_t byte_size = (int64_t)fs::file_size(abs_path);

            std::string skeleton;
            try {
                std::ifstream sf(abs_path, std::ios::binary);
                if (sf) {
                    std::ostringstream ss;
                    ss << sf.rdbuf();
                    skeleton = skeletonize(ss.str(), parsed->language);
                }
            } catch (...) {}

            int64_t fid = upsert_file(conn, parsed->path,
                                      language_name(parsed->language),
                                      parsed->hash, byte_size, skeleton);
            if (fid == -1) { stats.files_skipped++; continue; }  // unchanged

            insert_symbols(conn, fid, parsed->symbols);
            stats.symbols_found += (int)parsed->symbols.size();
            stats.edges_found   += (int)parsed->imports.size();
            stats.files_indexed++;

            if (!parsed->imports.empty())
                pending_edges.push_back({parsed->path, parsed->imports});
        }
        conn.Query("COMMIT");

        conn.Query("BEGIN TRANSACTION");
        for (const auto& p : pending_edges) {
            auto res = conn.Query("SELECT id FROM files WHERE path = '" + sq(p.path) + "'");
            auto& mat = *res;
            if (mat.RowCount() == 0) continue;
            int64_t fid = mat.GetValue<int64_t>(0, 0);
            resolve_edges(conn, fid, p.imports, cfg.project_cfg.granularity == "symbol");
        }
        conn.Query("COMMIT");
    }

    if (prune) stats.files_pruned = sweep_deleted(conn, cfg.project_root);

    return stats;
}

IndexStats sync_project(const Config& cfg, Database& db, ProgressCallback cb) {
    auto stats = index_project(cfg, db, cb);
    stats.files_pruned = sweep_deleted(db.conn(), cfg.project_root);
    return stats;
}

} // namespace axon
