#pragma once
#include <string>
#include <vector>
#include <filesystem>

// Forward declare llama types to avoid pulling the full header everywhere
struct llama_model;
struct llama_context;

namespace axon {

class Database;

class EmbeddingModel {
public:
    explicit EmbeddingModel(const std::filesystem::path& model_path);
    ~EmbeddingModel();

    // Returns a 768-dim float vector (nomic-embed-text-v1.5)
    std::vector<float> embed(const std::string& text);

    // Batch embed — more efficient than calling embed() in a loop
    std::vector<std::vector<float>> embed_batch(const std::vector<std::string>& texts);

    int dims() const { return dims_; }

private:
    llama_model*   model_   = nullptr;
    llama_context* ctx_     = nullptr;
    int            dims_    = 768;
};

// Cosine similarity between two equal-length vectors
float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b);

// Serialize/deserialize float vector to/from raw bytes (little-endian)
std::vector<uint8_t> serialize_embedding(const std::vector<float>& v);
std::vector<float>   deserialize_embedding(const uint8_t* data, size_t byte_len);

// Batch-embed all symbols that currently have embedding IS NULL.
// Returns the number of symbols embedded. Throws on DB errors.
//
// Used by both `axon index` (after full reindex) and the incremental
// `axon index-paths` path (after upserting specific files) so that any
// symbol just inserted becomes immediately searchable.
int embed_pending_symbols(Database& db, EmbeddingModel& model, int limit = 10000);

} // namespace axon
