#include "embeddings.hpp"
#include <filesystem>
#include "utf8.hpp"
#include <unordered_map>
#include <fstream>
#include "portfolio/domain/index_journal.hpp"
#include "db.hpp"
#include <ggml-backend.h>
#include <llama.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <thread>

namespace axon {

EmbeddingModel::EmbeddingModel(const std::filesystem::path& model_path) : path_(model_path) {
    std::string preference =
        std::getenv("AXON_EMBEDDING_DEVICE") ? std::getenv("AXON_EMBEDDING_DEVICE") : "auto";
    std::transform(preference.begin(), preference.end(), preference.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (preference != "auto" && preference != "cpu" && preference != "gpu") {
        throw std::invalid_argument("AXON_EMBEDDING_DEVICE must be auto, cpu, or gpu");
    }

    llama_backend_init();

    // Silence llama.cpp/ggml INFO/WARN chatter on stderr (e.g. the repeated
    // "cannot decode batches with this context" note during embedding). Keep
    // real errors visible.
    llama_log_set(
        [](ggml_log_level level, const char* text, void* /*ud*/) {
            if (level >= GGML_LOG_LEVEL_ERROR && text) fputs(text, stderr);
        },
        nullptr);

    llama_model_params mparams = llama_model_default_params();
    ggml_backend_dev_t selected_gpu = nullptr;
    if (preference != "cpu") {
        size_t best_free = 0;
        for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
            auto* device = ggml_backend_dev_get(i);
            const auto type = ggml_backend_dev_type(device);
            if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU)
                continue;
            // Virtualized GPUs (CI VMs, some hypervisors) lack features the Metal kernels rely
            // on: a Qwen3 decode aborts in ggml_metal_cpy_tensor_async (GGML_ASSERT(buf_dst)).
            // `auto` treats them as absent; `gpu` still uses them for whoever insists.
            const std::string description =
                ggml_backend_dev_description(device) ? ggml_backend_dev_description(device) : "";
            if (preference == "auto" && description.find("Paravirtual") != std::string::npos) {
                std::cerr << "[axon] ignoring virtualized GPU (" << description
                          << "); using CPU (AXON_EMBEDDING_DEVICE=gpu forces it)\n";
                continue;
            }
            size_t free_bytes = 0;
            size_t total_bytes = 0;
            ggml_backend_dev_memory(device, &free_bytes, &total_bytes);
            if (!selected_gpu ||
                (type == GGML_BACKEND_DEVICE_TYPE_GPU &&
                 ggml_backend_dev_type(selected_gpu) != GGML_BACKEND_DEVICE_TYPE_GPU) ||
                (type == ggml_backend_dev_type(selected_gpu) && free_bytes > best_free)) {
                selected_gpu = device;
                best_free = free_bytes;
            }
        }
    }
    if (preference == "gpu" && !selected_gpu) {
        llama_backend_free();
        throw std::runtime_error(
            "AXON_EMBEDDING_DEVICE=gpu requested, but no GPU backend/device is available");
    }

    ggml_backend_dev_t gpu_devices[] = {selected_gpu, nullptr};
    mparams.n_gpu_layers = selected_gpu ? -1 : 0;
    // An empty device list means CPU only. Leaving it unset would let llama.cpp offload large
    // batches to any registered GPU even with zero GPU layers.
    mparams.devices = selected_gpu ? gpu_devices : gpu_devices + 1;

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 512;
    cparams.n_batch = cparams.n_ctx;
    cparams.embeddings = true;
    {
        // The llama.cpp default (4) leaves most of a workstation idle during indexing.
        int threads = static_cast<int>(std::thread::hardware_concurrency()) / 2;
        threads = std::clamp(threads, 4, 8);
        if (const char* env = std::getenv("AXON_EMBEDDING_THREADS")) {
            try {
                threads = std::clamp(std::stoi(env), 1, 64);
            } catch (...) {
            }
        }
        cparams.n_threads = threads;
        cparams.n_threads_batch = threads;
    }

    auto load_model = [&]() {
        model_ = llama_model_load_from_file(model_path.string().c_str(), mparams);
        if (!model_) return false;
        ctx_ = llama_init_from_model(model_, cparams);
        if (ctx_) return true;
        llama_model_free(model_);
        model_ = nullptr;
        return false;
    };
    if (!load_model()) {
        if (preference != "auto" || !selected_gpu) {
            throw std::runtime_error("Failed to initialize embedding model on requested device: " +
                                     model_path.string());
        }
        std::cerr << "[axon] GPU embedding initialization failed; retrying on CPU\n";
        static ggml_backend_dev_t no_devices[] = {nullptr};
        mparams.devices = no_devices;
        mparams.n_gpu_layers = 0;
        if (!load_model()) {
            throw std::runtime_error("Failed to initialize embedding model on GPU or CPU: " +
                                     model_path.string());
        }
        selected_gpu = nullptr;
    }

    native_dims_ = llama_model_n_embd(model_);
    if (native_dims_ < kStoredEmbeddingDims) {
        llama_free(ctx_);
        llama_model_free(model_);
        ctx_ = nullptr;
        model_ = nullptr;
        throw std::runtime_error("Embedding model outputs " + std::to_string(native_dims_) +
                                 " dimensions; the index stores " +
                                 std::to_string(kStoredEmbeddingDims) +
                                 ". Use a model with at least that many dimensions.");
    }
    dims_ = kStoredEmbeddingDims;
    {
        auto meta = [&](const char* key) {
            char buf[256] = {0};
            const int n = llama_model_meta_val_str(model_, key, buf, sizeof(buf));
            return n > 0 ? std::string(buf) : std::string();
        };
        const std::string arch = meta("general.architecture");
        if (arch == "qwen3") {
            profile_ = {"qwen3-embedding",
                        "Instruct: Given a natural language question about a codebase, retrieve "
                        "the relevant function definition\nQuery: ",
                        "Instruct: Given a query, retrieve the relevant notes or conversation "
                        "turns\nQuery: ",
                        ""};
        } else if (arch == "gemma-embedding") {
            profile_ = {"embeddinggemma", "task: code retrieval | query: ",
                        "task: search result | query: ", "title: none | text: "};
        } else if (arch == "nomic-bert") {
            profile_ = {"nomic-embed-text-v1.5", "", "", ""};
        } else {
            std::string name = meta("general.name");
            profile_ = {arch + (name.empty() ? "" : ":" + name), "", "", ""};
        }
    }
    std::cerr << "[axon] embedding device: "
              << (selected_gpu ? ggml_backend_dev_name(selected_gpu) : "CPU") << "\n";
    std::cerr << "[axon] embedding model loaded, dims=" << dims_ << " (" << profile_.id << ")\n";
}

EmbeddingModel::~EmbeddingModel() {
    if (ctx_) llama_free(ctx_);
    if (model_) llama_model_free(model_);
    llama_backend_free();
}

std::vector<float> EmbeddingModel::embed(const std::string& text) {
    return embed_batch({text})[0];
}

std::vector<float> EmbeddingModel::embed_query(const std::string& text) {
    return embed(profile_.code_query + text);
}

std::vector<float> EmbeddingModel::embed_memory_query(const std::string& text) {
    return embed(profile_.memory_query + text);
}

std::vector<std::vector<float>>
EmbeddingModel::embed_documents(const std::vector<std::string>& texts) {
    if (profile_.document.empty()) return embed_batch(texts);
    std::vector<std::string> prefixed;
    prefixed.reserve(texts.size());
    for (const auto& text : texts)
        prefixed.push_back(profile_.document + text);
    return embed_batch(prefixed);
}

std::string EmbeddingModel::index_id() const {
    return profile_.id + "|" + std::to_string(dims_) + "|doc" +
           std::to_string(kSymbolDocumentRevision);
}

std::vector<std::vector<float>> EmbeddingModel::embed_batch(const std::vector<std::string>& texts) {
    std::vector<std::vector<float>> results;
    results.reserve(texts.size());

    const llama_vocab* vocab = llama_model_get_vocab(model_);

    for (const auto& text : texts) {
        std::vector<llama_token> tokens(512);
        int n = llama_tokenize(vocab, text.c_str(), (int32_t)text.size(), tokens.data(),
                               (int32_t)tokens.size(), true, false);
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

        // Matryoshka-trained models keep their quality when cut to the stored width.
        std::vector<float> vec(emb, emb + dims_);

        // L2 normalize
        float norm = 0.0f;
        for (float v : vec)
            norm += v * v;
        norm = std::sqrt(norm);
        if (norm > 1e-6f)
            for (float& v : vec)
                v /= norm;

        results.push_back(std::move(vec));
    }
    return results;
}

float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
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

namespace {

// Directory the project lives in: Axon keeps the index at <root>/.axon/index.duckdb.
std::filesystem::path project_root_of(Database& db) {
    const auto parent = db.path().parent_path();
    return parent.filename() == ".axon" ? parent.parent_path() : std::filesystem::path();
}

std::string trim_copy(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// Text embedded for one symbol: where it lives, what it is called (identifier split into
// words), its declaration, its documentation and the first lines of its body. The identifier
// alone is a poor retrieval target for natural-language questions.
std::string symbol_document(const std::string& path, const std::string& name,
                            const std::string& signature, const std::string& docstring,
                            const std::vector<std::string>* source_lines, int start_line,
                            int end_line) {
    std::string words;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '_') {
            words += ' ';
            continue;
        }
        if (i > 0 && std::isupper(static_cast<unsigned char>(c)) &&
            (std::islower(static_cast<unsigned char>(name[i - 1])) ||
             std::isdigit(static_cast<unsigned char>(name[i - 1]))))
            words += ' ';
        words += c;
    }
    std::string head;
    if (source_lines && start_line > 0) {
        int taken = 0;
        const int last = std::min<int>(end_line, static_cast<int>(source_lines->size()));
        // Skip the declaration line (already in the signature).
        for (int line = start_line + 1; line <= last && taken < 8; ++line) {
            const std::string text = trim_copy((*source_lines)[line - 1]);
            if (text.empty() || text.rfind("//", 0) == 0 || text.rfind("#", 0) == 0 ||
                text.rfind("*", 0) == 0 || text.rfind("/*", 0) == 0)
                continue;
            if (!head.empty()) head += " ; ";
            head += text;
            ++taken;
        }
    }
    std::string doc = path + " | " + words + " | " + trim_copy(signature) + " | " +
                      trim_copy(docstring).substr(0, 200) + " | " + head;
    if (doc.size() > 700) doc.resize(700);
    return doc;
}

// Symbols that answer "how does X work" questions. Variables, constants, signals, namespaces and
// the like stay in the lexical index only: as vectors they are noise that outranks real code.
constexpr const char* kEmbeddedKinds =
    "('function','async_function','method','constructor','class','partial_class','struct',"
    "'interface','enum','record','trait','protocol','object','mixin','extension','union','impl',"
    "'type','module')";

// Embeds up to `batch` pending symbols; returns how many were written.
int embed_symbol_batch(Database& db, EmbeddingModel& model, const std::filesystem::path& root,
                       int batch) {
    // Variables, fields, parameters and similar noise are left to lexical search.
    auto syms = db.conn().Query(
        "SELECT s.id, s.name, s.kind, COALESCE(s.signature,''), COALESCE(s.docstring,''), "
        "s.start_line, s.end_line, f.path FROM symbols s JOIN files f ON f.id = s.file_id "
        "WHERE s.embedding IS NULL AND (s.kind IN " +
        std::string(kEmbeddedKinds) +
        " OR (s.kind = 'constant' AND f.language IN ('typescript','javascript'))) "
        "ORDER BY s.id LIMIT " +
        std::to_string(batch));
    if (syms->HasError())
        throw std::runtime_error("Failed to fetch pending symbols: " + syms->GetError());
    if (syms->RowCount() == 0) return 0;

    std::vector<int64_t> ids;
    std::vector<std::string> texts;
    std::unordered_map<std::string, std::vector<std::string>> file_lines;
    for (duckdb::idx_t i = 0; i < syms->RowCount(); i++) {
        ids.push_back(syms->GetValue<int64_t>(0, i));
        const std::string path = syms->GetValue(7, i).ToString();
        const std::vector<std::string>* lines = nullptr;
        if (!root.empty()) {
            auto it = file_lines.find(path);
            if (it == file_lines.end()) {
                std::vector<std::string> loaded;
                std::ifstream in(root / path, std::ios::binary);
                std::string line;
                while (in && std::getline(in, line))
                    loaded.push_back(to_valid_utf8(line));
                it = file_lines.emplace(path, std::move(loaded)).first;
            }
            lines = &it->second;
        }
        texts.push_back(
            symbol_document(path, syms->GetValue(1, i).ToString(), syms->GetValue(3, i).ToString(),
                            syms->GetValue(4, i).ToString(), lines, syms->GetValue<int32_t>(5, i),
                            syms->GetValue<int32_t>(6, i)));
    }

    auto embeddings = model.embed_documents(texts);
    int dims = model.dims();
    portfolio::Transaction transaction(db.conn());
    transaction.mark_index_mutation();
    std::vector<portfolio::AffectedEntity> affected;

    for (size_t i = 0; i < ids.size(); i++) {
        std::ostringstream sql;
        sql << "UPDATE symbols SET embedding = [";
        const auto& emb = embeddings[i];
        for (size_t j = 0; j < emb.size(); j++) {
            if (j) sql << ",";
            sql << emb[j];
        }
        sql << "]::FLOAT[" << dims << "] WHERE id = " << ids[i];
        auto r = db.conn().Query(sql.str());
        if (r->HasError())
            throw std::runtime_error("UPDATE symbols failed (id=" + std::to_string(ids[i]) +
                                     "): " + r->GetError());
        affected.push_back({"symbol", std::to_string(ids[i]), "upsert", std::nullopt});
    }
    portfolio::trigger_journal_failpoint_for_testing("after_mutation");
    const std::string manifest = portfolio::compute_manifest_hash(db.conn());
    portfolio::append_index_events(transaction, db.conn(), "IndexSymbolsUpdated", affected,
                                   manifest);
    transaction.commit();
    return static_cast<int>(ids.size());
}

} // namespace

bool embedding_model_matches(Database& db, const EmbeddingModel& model) {
    const std::string stored = stored_embedding_model_id(db);
    if (stored.empty())
        return !any_stored_embedding_vectors(db) || model.profile().id == "nomic-embed-text-v1.5";
    return stored == model.index_id();
}

bool ensure_embedding_model(Database& db, const EmbeddingModel& model) {
    const std::string current = model.index_id();
    const std::string stored = stored_embedding_model_id(db);
    if (stored == current) return false;
    // Legacy indexes (no recorded identity) hold nomic vectors from before profiles existed; they
    // stay valid for the nomic model and are cleared for any other.
    const bool legacy_compatible = stored.empty() && model.profile().id == "nomic-embed-text-v1.5";
    const bool had_vectors =
        legacy_compatible ? false : (stored.empty() ? any_stored_embedding_vectors(db) : true);
    if (had_vectors) {
        for (const char* sql :
             {"UPDATE symbols SET embedding = NULL WHERE embedding IS NOT NULL",
              "UPDATE turns SET embedding = NULL WHERE embedding IS NOT NULL",
              "UPDATE observations SET embedding = NULL WHERE embedding IS NOT NULL",
              "UPDATE sessions SET digest_embedding = NULL WHERE digest_embedding IS NOT NULL",
              "DELETE FROM capsule_cache"}) {
            auto result = db.conn().Query(sql);
            if (result->HasError())
                throw std::runtime_error(std::string("embedding model switch failed: ") +
                                         result->GetError());
        }
        std::cerr << "[axon] embedding model changed (" << (stored.empty() ? "legacy" : stored)
                  << " -> " << current << "); vectors will be rebuilt\n";
    }
    auto recorded = db.conn().Query(
        "INSERT INTO embedding_state(singleton, model_id) VALUES (true, '" + current +
        "') ON CONFLICT (singleton) DO UPDATE SET model_id = excluded.model_id, "
        "updated_at = now()");
    if (recorded->HasError())
        throw std::runtime_error("recording embedding model failed: " + recorded->GetError());
    return had_vectors;
}

int embed_pending_symbols(Database& db, EmbeddingModel& model, int limit) {
    ensure_embedding_model(db, model);
    const auto root = project_root_of(db);
    constexpr int kBatch = 256;
    int total = 0;
    while (total < limit) {
        const int embedded = embed_symbol_batch(db, model, root, std::min(kBatch, limit - total));
        if (embedded == 0) break;
        total += embedded;
    }
    return total;
}

} // namespace axon
