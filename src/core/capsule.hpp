#pragma once
#include "db.hpp"
#include "graph.hpp"
#include "embeddings.hpp"
#include "compress.hpp"
#include "../parser/parser.hpp"
#include <string>
#include <vector>
#include <filesystem>

namespace axon {

struct CapsuleFile {
    std::string path;
    std::string source_ref;
    std::string expand_command;
    std::string content;
    bool is_skeleton = false;
    int token_estimate = 0;
};

struct DialogueTurn {
    std::string role;
    std::string content;
    std::string session_label;
    std::string thread_name;
    std::string ts;
    int token_estimate = 0;
};

struct ContextCapsule {
    std::string query;
    std::vector<CapsuleFile> pivot_files;
    std::vector<CapsuleFile> support_files;
    std::vector<DialogueTurn> related_turns; // populated when dialogue_budget > 0
    int token_estimate = 0;
    int total_files = 0;
    int compression_input_tokens = 0;
    int compression_output_tokens = 0;
    int compression_tokens_saved = 0;
    std::vector<std::string> ccr_artifact_ids;
};

inline int estimate_tokens(const std::string& s) {
    return (int)((s.size() + 3) / 4);
}

ContextCapsule
assemble_capsule(const std::string& query,
                 const std::vector<std::string>& explicit_pivots, // empty = use query
                 Database& db, EmbeddingModel& model, const DependencyGraph& graph,
                 const std::filesystem::path& project_root, int token_budget = 8000,
                 int dialogue_budget = 0, // 0 = disabled; >0 = pull anchored turns into capsule
                 CapsuleCompression compression = CapsuleCompression::Off); // Balde A opt-in

// ── Cache primitives (W2.T01) ───────────────────────────────────────────────
// Cache key = BLAKE3(query + "|" + token_budget + "|" + project_epoch),
// where project_epoch = string-formatted max(files.indexed_at). Hits avoid
// the embedding + ranking + skeletonization roundtrip (~10x faster on
// repeat queries against an unchanged index).

// Snapshot the current index epoch. Returns "0" when no files indexed yet.
std::string current_project_epoch(Database& db);

// BLAKE3-hash the cache key inputs. Pure function, no DB access.
std::string compute_capsule_cache_key(const std::string& query, int token_budget,
                                      const std::string& epoch);

// Look up a cached capsule. Returns nullopt on miss or epoch mismatch.
std::optional<ContextCapsule> capsule_cache_lookup(Database& db, const std::string& key,
                                                   const std::string& epoch);

// Persist a capsule under the given key+epoch. Best-effort: a failed write
// is logged to stderr but does not throw.
void capsule_cache_insert(Database& db, const std::string& key, const std::string& epoch,
                          const ContextCapsule& capsule);

} // namespace axon
