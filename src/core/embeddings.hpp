#pragma once
#include "embedding_state.hpp"
#include <string>
#include <vector>
#include <filesystem>

// Forward declare llama types to avoid pulling the full header everywhere
struct llama_model;
struct llama_context;

namespace axon {

class Database;

// Dimension of every stored vector (FLOAT[768] columns). Models with wider output that were
// trained with Matryoshka representation learning (e.g. Qwen3-Embedding) are truncated to this
// size and re-normalized; narrower models are rejected.
constexpr int kStoredEmbeddingDims = 768;

// Per-architecture conventions: asymmetric retrieval models need task prefixes on queries (and
// sometimes documents). Without them retrieval quality drops sharply.
struct EmbeddingProfile {
    std::string id;           // stable identity, recorded in the index
    std::string code_query;   // prefix for natural-language -> code queries
    std::string memory_query; // prefix for queries over notes / conversation turns
    std::string document;     // prefix for indexed text
};

class EmbeddingModel {
public:
    explicit EmbeddingModel(const std::filesystem::path& model_path);
    ~EmbeddingModel();

    // Raw embedding of `text` exactly as given (no task prefix), kStoredEmbeddingDims wide.
    std::vector<float> embed(const std::string& text);

    // Batch embed — more efficient than calling embed() in a loop
    std::vector<std::vector<float>> embed_batch(const std::vector<std::string>& texts);

    // Query-side embeddings with the profile's task prefix.
    std::vector<float> embed_query(const std::string& text);        // code search
    std::vector<float> embed_memory_query(const std::string& text); // notes / dialogue turns

    // Document-side batch embedding with the profile's document prefix.
    std::vector<std::vector<float>> embed_documents(const std::vector<std::string>& texts);

    int dims() const { return dims_; }
    const EmbeddingProfile& profile() const { return profile_; }
    const std::filesystem::path& path() const { return path_; }

    // Identity of the vector space: profile + stored dims + document-text revision. Vectors
    // produced under different identities are not comparable.
    std::string index_id() const;

private:
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    int dims_ = kStoredEmbeddingDims;
    int native_dims_ = kStoredEmbeddingDims;
    EmbeddingProfile profile_;
    std::filesystem::path path_;
};

// Revision of the text built for each symbol (path, name, signature, docs, body head). Bump when
// the format changes so existing vectors are rebuilt.
constexpr int kSymbolDocumentRevision = 3;

// Records which embedding model produced the stored vectors. When it differs from `model`,
// every stored vector (symbols, turns, session digests, observations) is cleared, the capsule
// cache is dropped and the new identity is recorded. Returns true when vectors were cleared and
// need to be rebuilt.
bool ensure_embedding_model(Database& db, const EmbeddingModel& model);

// True when stored vectors (if any) were produced by `model`. Query paths use this to refuse
// comparing vectors from different spaces.
bool embedding_model_matches(Database& db, const EmbeddingModel& model);

// Cosine similarity between two equal-length vectors
float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b);

// Serialize/deserialize float vector to/from raw bytes (little-endian)
std::vector<uint8_t> serialize_embedding(const std::vector<float>& v);
std::vector<float> deserialize_embedding(const uint8_t* data, size_t byte_len);

// Batch-embed all symbols that currently have embedding IS NULL.
// Returns the number of symbols embedded. Throws on DB errors.
//
// Used by both `axon index` (after full reindex) and the incremental
// `axon index-paths` path (after upserting specific files) so that any
// symbol just inserted becomes immediately searchable.
//
// Processes pending symbols in bounded batches until none are left (or `limit` is reached), so a
// repository with tens of thousands of symbols is fully embedded in one call. Switches the index
// to `model` first (see ensure_embedding_model).
int embed_pending_symbols(Database& db, EmbeddingModel& model, int limit = 1000000);

} // namespace axon
