#include "embeddings.hpp"
#include "db.hpp"
#include <llama.h>
#include <stdexcept>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>

namespace axon {

EmbeddingModel::EmbeddingModel(const std::filesystem::path& model_path) {
    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;

    model_ = llama_model_load_from_file(model_path.string().c_str(), mparams);
    if (!model_)
        throw std::runtime_error("Failed to load embedding model: " + model_path.string());

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx       = 512;
    cparams.n_batch     = cparams.n_ctx;
    cparams.embeddings  = true;

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_)
        throw std::runtime_error("Failed to create llama context");

    dims_ = llama_model_n_embd(model_);
    std::cerr << "[axon] embedding model loaded, dims=" << dims_ << "\n";
}

EmbeddingModel::~EmbeddingModel() {
    if (ctx_)   llama_free(ctx_);
    if (model_) llama_model_free(model_);
    llama_backend_free();
}

std::vector<float> EmbeddingModel::embed(const std::string& text) {
    return embed_batch({text})[0];
}

std::vector<std::vector<float>> EmbeddingModel::embed_batch(const std::vector<std::string>& texts) {
    std::vector<std::vector<float>> results;
    results.reserve(texts.size());

    const llama_vocab* vocab = llama_model_get_vocab(model_);

    for (const auto& text : texts) {
        std::vector<llama_token> tokens(512);
        int n = llama_tokenize(vocab, text.c_str(), (int32_t)text.size(),
                               tokens.data(), (int32_t)tokens.size(),
                               true, false);
        if (n < 0) {
            results.push_back(std::vector<float>(dims_, 0.0f));
            continue;
        }
        tokens.resize(n);

        llama_memory_clear(llama_get_memory(ctx_), false);

        llama_batch batch = llama_batch_get_one(tokens.data(), (int32_t)tokens.size());
        if (llama_decode(ctx_, batch) != 0) {
            results.push_back(std::vector<float>(dims_, 0.0f));
            continue;
        }

        const float* emb = llama_get_embeddings_seq(ctx_, 0);
        if (!emb) emb = llama_get_embeddings_ith(ctx_, (int32_t)tokens.size() - 1);

        std::vector<float> vec(emb, emb + dims_);

        // L2 normalize
        float norm = 0.0f;
        for (float v : vec) norm += v * v;
        norm = std::sqrt(norm);
        if (norm > 1e-6f)
            for (float& v : vec) v /= norm;

        results.push_back(std::move(vec));
    }
    return results;
}

float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    float denom = std::sqrt(na) * std::sqrt(nb);
    return denom < 1e-6f ? 0.0f : dot / denom;
}

std::vector<uint8_t> serialize_embedding(const std::vector<float>& v) {
    std::vector<uint8_t> bytes(v.size() * 4);
    std::memcpy(bytes.data(), v.data(), bytes.size());
    return bytes;
}

std::vector<float> deserialize_embedding(const uint8_t* data, size_t byte_len) {
    std::vector<float> v(byte_len / 4);
    std::memcpy(v.data(), data, byte_len);
    return v;
}

int embed_pending_symbols(Database& db, EmbeddingModel& model, int limit) {
    auto sq2 = [](const std::string& s) {
        std::string r; r.reserve(s.size());
        for (char c : s) { if (c == '\'') r += "''"; else r += c; }
        return r;
    };

    auto syms = db.conn().Query(
        "SELECT id, name, kind, signature "
        "FROM symbols WHERE embedding IS NULL LIMIT " + std::to_string(limit));
    if (syms->HasError())
        throw std::runtime_error("Failed to fetch pending symbols: " + syms->GetError());

    std::vector<int64_t>     ids;
    std::vector<std::string> texts;
    for (duckdb::idx_t i = 0; i < syms->RowCount(); i++) {
        ids.push_back(syms->GetValue<int64_t>(0, i));
        texts.push_back(syms->GetValue(1, i).ToString() + " " +
                        syms->GetValue(2, i).ToString() + " " +
                        syms->GetValue(3, i).ToString());
    }

    if (texts.empty()) return 0;

    auto embeddings = model.embed_batch(texts);
    int  dims       = model.dims();

    for (size_t i = 0; i < ids.size(); i++) {
        std::ostringstream sql;
        sql << "UPDATE symbols SET embedding = [";
        const auto& emb = embeddings[i];
        for (size_t j = 0; j < emb.size(); j++) { if (j) sql << ","; sql << emb[j]; }
        sql << "]::FLOAT[" << dims << "] WHERE id = " << ids[i];
        auto r = db.conn().Query(sql.str());
        if (r->HasError())
            throw std::runtime_error("UPDATE symbols failed (id=" + std::to_string(ids[i]) + "): " + r->GetError());
    }
    return (int)ids.size();
}

} // namespace axon
