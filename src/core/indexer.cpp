#include "indexer.hpp"
#include "skeleton.hpp"
#include "../parser/parser.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <vector>
#include <sstream>

// Escape single quotes for inline SQL strings
static std::string sq(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out;
}

namespace axon {
namespace fs = std::filesystem;

// Hard skip-list lives in skip_dirs.cpp (is_hard_skip_dir) so the native
// watchers and their tests share it without linking the indexer.

// Compiled .axonignore entry. Patterns with no glob meta-chars retain a fast
// equality path; patterns containing *, **, or ? compile to regex once and
// match repeatedly. Last-rule-wins for conflicting include/exclude (gitignore
// semantics).
struct IgnoreRule {
    std::string raw;       // original line, post-trim
    bool negate = false;   // leading `!` flips include/exclude
    bool anchored = false; // leading `/` matches only at project root
    bool dir_only = false; // trailing `/` matches directories only
    bool has_glob = false; // any of * ? **
    std::regex re;         // compiled if has_glob, else unused
};

static std::vector<IgnoreRule> g_ignore_rules;
static fs::path g_project_root;

// Translate a single gitignore-style glob pattern into an anchored ECMAScript
// regex. Honors `*` (no slashes), `**` (any), `?` (one char no slash), and
// escapes regex meta-chars. The resulting regex matches the *full* candidate
// string, so callers should pass either a filename or a project-relative path
// based on the rule's `anchored` flag.
static std::regex glob_to_regex(const std::string& pat) {
    std::string re;
    re.reserve(pat.size() * 2 + 8);

    // Gitignore semantics for a leading `**/`: matches zero or more leading
    // directory segments. Without this special case `**/foo` would require
    // *something* before /foo and miss top-level `foo`. Translate the prefix
    // to `(?:.*/)?` (optional path prefix) and skip the literal slash that
    // follows so we don't accidentally require a leading separator.
    size_t start = 0;
    if (pat.size() >= 3 && pat.compare(0, 3, "**/") == 0) {
        re += "^(?:.*/)?";
        start = 3;
    } else {
        re += "^";
    }

    for (size_t i = start; i < pat.size(); i++) {
        char c = pat[i];
        if (c == '*') {
            if (i + 1 < pat.size() && pat[i + 1] == '*') {
                // Mid-pattern `**` — match anything across slashes.
                re += ".*";
                i++; // consume the second *
            } else {
                re += "[^/]*";
            }
        } else if (c == '?') {
            re += "[^/]";
        } else if (c == '.' || c == '+' || c == '(' || c == ')' || c == '[' || c == ']' ||
                   c == '{' || c == '}' || c == '^' || c == '$' || c == '|' || c == '\\') {
            re += '\\';
            re += c;
        } else {
            re += c;
        }
    }
    re += "$";
    return std::regex(re);
}

static void load_axonignore(const fs::path& project_root) {
    g_ignore_rules.clear();
    g_project_root = project_root;
    auto ignore_file = project_root / ".axonignore";
    std::ifstream f(ignore_file);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        // Strip trailing whitespace + CR
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        // Skip empty + comment lines
        if (line.empty() || line[0] == '#') continue;
        IgnoreRule rule;
        if (!line.empty() && line[0] == '!') {
            rule.negate = true;
            line = line.substr(1);
        }
        if (!line.empty() && line[0] == '/') {
            rule.anchored = true;
            line = line.substr(1);
        }
        if (!line.empty() && line.back() == '/') {
            rule.dir_only = true;
            line.pop_back();
        }
        if (line.empty()) continue;
        rule.raw = line;
        rule.has_glob =
            (line.find('*') != std::string::npos || line.find('?') != std::string::npos);
        if (rule.has_glob) {
            try {
                rule.re = glob_to_regex(line);
            } catch (...) {
                std::cerr << "[warn] .axonignore: invalid pattern '" << line << "', skipping\n";
                continue;
            }
        }
        g_ignore_rules.push_back(std::move(rule));
    }
}

static bool should_skip(const fs::path& p) {
    // Hard-coded skip dirs always apply, regardless of .axonignore — these
    // are not user-overridable for safety (re-indexing a node_modules would
    // tank performance and pollute the symbol space).
    if (is_hard_skip_dir(p.filename().string())) return true;
    if (g_ignore_rules.empty()) return false;
    // Compute filename + project-relative path once.
    std::string fname = p.filename().string();
    std::string rel;
    {
        std::error_code ec;
        auto rel_path = fs::relative(p, g_project_root, ec);
        rel = ec ? fname : rel_path.generic_string();
    }
    bool is_dir = fs::is_directory(p);
    // gitignore semantics: last matching rule wins. Default = not skipped;
    // a non-negated rule excludes, a negated rule re-includes.
    bool skipped = false;
    for (const auto& rule : g_ignore_rules) {
        if (rule.dir_only && !is_dir) continue;
        bool hit = false;
        if (rule.has_glob) {
            // Anchored rules match against the project-relative path; bare
            // patterns also match if any path segment matches the filename
            // (gitignore convention for `*.log` etc.)
            if (std::regex_match(rel, rule.re))
                hit = true;
            else if (!rule.anchored && std::regex_match(fname, rule.re))
                hit = true;
        } else {
            // Plain string: anchored vs filename-eq, matches v0.5.0 behavior.
            if (rule.anchored)
                hit = (rel == rule.raw);
            else
                hit = (fname == rule.raw);
        }
        if (hit) skipped = !rule.negate;
    }
    return skipped;
}

// Returns file id, or -1 if unchanged (same hash and !force)
static int64_t upsert_file(duckdb::Connection& conn, const std::string& rel_path,
                           const std::string& lang, const std::string& hash, int64_t byte_size,
                           const std::string& skeleton, bool force = false) {
    // Check existing
    auto check = conn.Query("SELECT id, hash FROM files WHERE path = '" + sq(rel_path) + "'");
    auto& mat = *check;

    if (mat.RowCount() > 0) {
        int64_t fid = mat.GetValue<int64_t>(0, 0);
        std::string existing_hash = mat.GetValue(1, 0).ToString();
        if (existing_hash == hash && !force) return -1; // unchanged

        // Updated: delete this file's outgoing edges (will be rebuilt) + symbols.
        // Keep incoming edges — those belong to OTHER files' resolve_edges results
        // and would be lost here since those files aren't being re-walked.
        conn.Query("DELETE FROM edges WHERE from_file = " + std::to_string(fid));
        conn.Query("DELETE FROM symbols WHERE file_id = " + std::to_string(fid));
        conn.Query("UPDATE files SET hash = '" + sq(hash) + "', language = '" + sq(lang) +
                   "', byte_size = " + std::to_string(byte_size) + ", skeleton = '" + sq(skeleton) +
                   "', indexed_at = now() WHERE id = " + std::to_string(fid));
        return fid;
    }

    // New file
    auto res =
        conn.Query("INSERT INTO files (id, path, language, hash, byte_size, skeleton, indexed_at) "
                   "VALUES (nextval('seq_id'), '" +
                   sq(rel_path) + "', '" + sq(lang) + "', '" + sq(hash) + "', " +
                   std::to_string(byte_size) + ", '" + sq(skeleton) + "', now()) RETURNING id");
    auto& mat2 = *res;
    return mat2.GetValue<int64_t>(0, 0);
}

static void insert_symbols(duckdb::Connection& conn, int64_t file_id,
                           const std::vector<Symbol>& symbols) {
    for (const auto& sym : symbols) {
        std::string sql = "INSERT INTO symbols (id, file_id, name, kind, start_line, end_line, "
                          "signature, docstring) "
                          "VALUES (nextval('seq_id'), " +
                          std::to_string(file_id) + ", '" + sq(sym.name) + "', '" + sq(sym.kind) +
                          "', " + std::to_string(sym.start_line) + ", " +
                          std::to_string(sym.end_line) + ", '" + sq(sym.signature.value_or("")) +
                          "', '" + sq(sym.docstring.value_or("")) + "')";
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
static int64_t resolve_specifier_to_file(duckdb::Connection& conn, const std::string& specifier,
                                         int64_t from_id) {
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
        std::string sql = "SELECT id FROM files WHERE id != " + std::to_string(from_id) +
                          " AND ("
                          "  path LIKE '%/" +
                          n +
                          ".%'"
                          " OR path LIKE '" +
                          n +
                          ".%'"
                          " OR path LIKE '%/" +
                          n +
                          "/index.%'"
                          " OR path LIKE '%/" +
                          n +
                          "/mod.%'" // Rust module file convention
                          " OR path LIKE '%/" +
                          n +
                          ".d.ts'" // TS declaration
                          ") LIMIT 1";
        return run(sql);
    };

    // try_contains: last-resort substring match (loose, may overmatch).
    auto try_contains = [&](const std::string& needle) -> int64_t {
        if (needle.size() < 3) return 0;
        return run("SELECT id FROM files WHERE path LIKE '%" + sq(needle) +
                   "%' "
                   "AND id != " +
                   std::to_string(from_id) + " LIMIT 1");
    };

    // Normalize: strip leading "./", "../" chain, and surrounding whitespace.
    std::string s = specifier;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(0, 1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    while (s.size() >= 2 && s[0] == '.' && (s[1] == '/' || s[1] == '.')) {
        size_t cut = s.find('/');
        if (cut == std::string::npos) break;
        s = s.substr(cut + 1);
    }
    if (s.empty()) return 0;

    // 1) If specifier contains '/', prefer suffix path match (likely already a relative path).
    if (s.find('/') != std::string::npos) {
        int64_t id = run("SELECT id FROM files WHERE path LIKE '%/" + sq(s) +
                         ".%' "
                         "AND id != " +
                         std::to_string(from_id) + " LIMIT 1");
        if (!id)
            id = run("SELECT id FROM files WHERE path LIKE '%" + sq(s) +
                     ".%' "
                     "AND id != " +
                     std::to_string(from_id) + " LIMIT 1");
        if (id) return id;
    }

    // 2) Tokenize on separators (., /, \, ::)
    std::vector<std::string> parts;
    {
        std::string cur;
        for (size_t i = 0; i < s.size();) {
            char c = s[i];
            if (c == ':' && i + 1 < s.size() && s[i + 1] == ':') {
                if (!cur.empty()) {
                    parts.push_back(cur);
                    cur.clear();
                }
                i += 2;
                continue;
            }
            if (c == '.' || c == '/' || c == '\\') {
                if (!cur.empty()) {
                    parts.push_back(cur);
                    cur.clear();
                }
                i++;
                continue;
            }
            cur += c;
            i++;
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
                          const std::vector<ImportEdge>& imports, bool symbol_mode) {
    if (imports.empty()) return;
    for (const auto& edge : imports) {
        int64_t to_id = resolve_specifier_to_file(conn, edge.to_specifier, from_id);
        if (to_id == 0) continue;

        // Insert file-level edge
        auto ins = conn.Query(
            "INSERT INTO edges (id, from_file, to_file, kind) VALUES (nextval('seq_id'), " +
            std::to_string(from_id) + ", " + std::to_string(to_id) + ", '" + sq(edge.kind) +
            "') RETURNING id");

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
        auto from_sym =
            conn.Query("SELECT id FROM symbols WHERE file_id = " + std::to_string(from_id) +
                       " AND name = '" + sq(spec) + "' LIMIT 1");
        int64_t from_sym_id = (!from_sym->HasError() && from_sym->RowCount() > 0)
                                  ? from_sym->GetValue<int64_t>(0, 0)
                                  : 0;

        // Try to find a symbol in to_file with matching name
        auto to_sym = conn.Query("SELECT id FROM symbols WHERE file_id = " + std::to_string(to_id) +
                                 " AND name = '" + sq(spec) + "' LIMIT 1");
        int64_t to_sym_id =
            (!to_sym->HasError() && to_sym->RowCount() > 0) ? to_sym->GetValue<int64_t>(0, 0) : 0;

        if (from_sym_id > 0 || to_sym_id > 0) {
            std::string upd = "UPDATE edges SET ";
            if (from_sym_id > 0) upd += "from_symbol = " + std::to_string(from_sym_id);
            if (from_sym_id > 0 && to_sym_id > 0) upd += ", ";
            if (to_sym_id > 0) upd += "to_symbol = " + std::to_string(to_sym_id);
            upd += " WHERE id = " + std::to_string(edge_id);
            conn.Query(upd);
        }
    }
}

// For each call site (caller_name → callee_name) inside file `from_id`,
// emit a symbol-to-symbol edge `kind=calls`. Resolution rules:
//   from_symbol = unique symbol named caller_name in this file
//   to_symbol   = symbol named callee_name in any file (prefer non-test files;
//                 if multiple matches, take the one with most callers — most
//                 likely the canonical definition)
// Drops calls that cannot be resolved on either side.
static void resolve_calls(duckdb::Connection& conn, int64_t from_id,
                          const std::vector<CallSite>& calls) {
    if (calls.empty()) return;

    for (const auto& c : calls) {
        if (c.caller_name.empty() || c.callee_name.empty()) continue;

        auto from_res =
            conn.Query("SELECT id FROM symbols WHERE file_id = " + std::to_string(from_id) +
                       " AND name = '" + sq(c.caller_name) + "' LIMIT 1");
        if (from_res->HasError() || from_res->RowCount() == 0) continue;
        int64_t from_sym_id = from_res->GetValue<int64_t>(0, 0);

        // Look up callee globally. Prefer same file (intra-module call), then
        // any other file. Pick the most-defined one when multiple match.
        auto to_res = conn.Query("SELECT s.id, s.file_id FROM symbols s "
                                 "WHERE s.name = '" +
                                 sq(c.callee_name) +
                                 "' "
                                 "ORDER BY (s.file_id = " +
                                 std::to_string(from_id) +
                                 ") DESC, s.id ASC "
                                 "LIMIT 1");
        if (to_res->HasError() || to_res->RowCount() == 0) continue;
        int64_t to_sym_id = to_res->GetValue<int64_t>(0, 0);
        int64_t to_file_id = to_res->GetValue<int64_t>(1, 0);
        if (to_sym_id == from_sym_id) continue; // self-recursion, drop

        conn.Query(
            "INSERT INTO edges (id, from_file, to_file, kind, from_symbol, to_symbol) VALUES (" +
            std::string("nextval('seq_id'), ") + std::to_string(from_id) + ", " +
            std::to_string(to_file_id) + ", 'calls', " + std::to_string(from_sym_id) + ", " +
            std::to_string(to_sym_id) + ")");
    }
}

// Returns true if any component of path matches a skip dir or ignore pattern.
// Hooked into sweep_deleted: a previously-indexed file becomes ignored when
// the user adds a matching .axonignore rule, and we want it pruned on the
// next indexing pass even though the file still exists on disk.
static bool path_is_ignored(const fs::path& rel) {
    for (const auto& part : rel) {
        if (is_hard_skip_dir(part.filename().string())) return true;
    }
    // Match against the compiled .axonignore rules: we only need a pruning
    // signal here, so reuse the same rule semantics as the walker — anchored
    // patterns get the project-relative path; bare patterns also test against
    // the basename. Negation rules can re-include.
    if (g_ignore_rules.empty()) return false;
    const std::string fname = rel.filename().string();
    const std::string rel_str = rel.generic_string();
    bool ignored = false;
    for (const auto& rule : g_ignore_rules) {
        bool hit = false;
        if (rule.has_glob) {
            if (std::regex_match(rel_str, rule.re))
                hit = true;
            else if (!rule.anchored && std::regex_match(fname, rule.re))
                hit = true;
        } else {
            if (rule.anchored)
                hit = (rel_str == rule.raw);
            else
                hit = (fname == rule.raw);
        }
        if (hit) ignored = !rule.negate;
    }
    return ignored;
}

// Remove files from DB whose path no longer exists on disk OR whose path is now ignored.
// Also cleans up dependent symbols and edges. Returns how many files were pruned.
static int sweep_deleted(duckdb::Connection& conn, const fs::path& project_root) {
    auto res = conn.Query("SELECT id, path FROM files");
    if (res->HasError()) return 0;
    auto& mat = *res;

    struct Victim {
        int64_t id;
        std::string path;
    };
    std::vector<Victim> victims;
    for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
        int64_t fid = mat.GetValue<int64_t>(0, i);
        std::string rel = mat.GetValue(1, i).ToString();
        fs::path abs = project_root / rel;
        if (!fs::exists(abs) || path_is_ignored(fs::path(rel))) victims.push_back({fid, rel});
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

IndexStats index_project(const Config& cfg, Database& db, ProgressCallback on_progress,
                         bool force) {
    IndexStats stats;
    auto& conn = db.conn();

    load_axonignore(cfg.project_root);

    // Collect source files
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(cfg.project_root,
                                                    fs::directory_options::skip_permission_denied);
         it != fs::end(it); ++it) {
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
    struct Pending {
        std::string path;
        std::vector<ImportEdge> imports;
        std::vector<CallSite> calls;
    };
    std::vector<Pending> pending_edges;

    conn.Query("BEGIN TRANSACTION");
    for (int i = 0; i < total; i++) {
        const auto& abs_path = files[i];
        if (on_progress) on_progress(abs_path.filename().string(), i, total);

        auto parsed = parse_file(abs_path, cfg.project_root);
        if (!parsed) {
            stats.files_skipped++;
            continue;
        }

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
        } catch (...) {
        }

        int64_t fid = upsert_file(conn, parsed->path, language_name(parsed->language), parsed->hash,
                                  byte_size, skeleton, force);
        if (fid == -1) {
            stats.files_skipped++;
            continue;
        } // unchanged

        insert_symbols(conn, fid, parsed->symbols);
        stats.symbols_found += (int)parsed->symbols.size();
        stats.edges_found += (int)parsed->imports.size();
        stats.files_indexed++;

        if (!parsed->imports.empty() || !parsed->calls.empty())
            pending_edges.push_back({parsed->path, parsed->imports, parsed->calls});
    }
    conn.Query("COMMIT");

    bool symbol_mode = cfg.project_cfg.granularity == "symbol";

    // Resolve edges (second pass — all files now in DB)
    conn.Query("BEGIN TRANSACTION");
    for (const auto& p : pending_edges) {
        auto res = conn.Query("SELECT id FROM files WHERE path = '" + sq(p.path) + "'");
        auto& mat = *res;
        if (mat.RowCount() == 0) continue;
        int64_t fid = mat.GetValue<int64_t>(0, 0);
        resolve_edges(conn, fid, p.imports, symbol_mode);
        if (symbol_mode && !p.calls.empty()) {
            resolve_calls(conn, fid, p.calls);
        }
    }
    conn.Query("COMMIT");

    // Prune deleted and newly-ignored files
    stats.files_pruned = sweep_deleted(conn, cfg.project_root);

    return stats;
}

IndexStats index_files(const Config& cfg, Database& db, const std::vector<fs::path>& paths,
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
        if (ec || rel.empty() || rel.generic_string().rfind("..", 0) == 0) continue;

        auto ext = abs.extension().string();
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        if (!language_from_extension(ext)) continue;

        abs_paths.push_back(abs);
    }

    int total = (int)abs_paths.size();

    struct Pending {
        std::string path;
        std::vector<ImportEdge> imports;
    };
    std::vector<Pending> pending_edges;

    if (total > 0) {
        conn.Query("BEGIN TRANSACTION");
        for (int i = 0; i < total; i++) {
            const auto& abs_path = abs_paths[i];
            if (on_progress) on_progress(abs_path.filename().string(), i, total);

            auto parsed = parse_file(abs_path, cfg.project_root);
            if (!parsed) {
                stats.files_skipped++;
                continue;
            }

            int64_t byte_size = (int64_t)fs::file_size(abs_path);

            std::string skeleton;
            try {
                std::ifstream sf(abs_path, std::ios::binary);
                if (sf) {
                    std::ostringstream ss;
                    ss << sf.rdbuf();
                    skeleton = skeletonize(ss.str(), parsed->language);
                }
            } catch (...) {
            }

            int64_t fid = upsert_file(conn, parsed->path, language_name(parsed->language),
                                      parsed->hash, byte_size, skeleton);
            if (fid == -1) {
                stats.files_skipped++;
                continue;
            } // unchanged

            insert_symbols(conn, fid, parsed->symbols);
            stats.symbols_found += (int)parsed->symbols.size();
            stats.edges_found += (int)parsed->imports.size();
            stats.files_indexed++;

            if (!parsed->imports.empty()) pending_edges.push_back({parsed->path, parsed->imports});
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
