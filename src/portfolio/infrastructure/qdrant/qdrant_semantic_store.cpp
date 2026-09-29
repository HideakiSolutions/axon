#include "qdrant_semantic_store.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <blake3.h>
#include <stdexcept>
#include <cctype>
namespace axon::portfolio {
namespace {
using json = nlohmann::json;
size_t sink(char* p, size_t s, size_t n, void* u) {
    static_cast<std::string*>(u)->append(p, s * n);
    return s * n;
}
std::string call(const std::string& url, const std::string& key, const std::string& method,
                 const json& body) {
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("Qdrant unavailable");
    std::string out;
    curl_slist* h = nullptr;
    h = curl_slist_append(h, "Content-Type: application/json");
    h = curl_slist_append(h, ("api-key: " + key).c_str());
    auto data = body.dump();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 3L);
    auto rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (rc || code < 200 || code >= 300) throw std::runtime_error("Qdrant request failed");
    return out;
}
json vec(const std::vector<float>& v) {
    return json(v);
}
void hash_field(blake3_hasher& h, const std::string& value) {
    const auto length = std::to_string(value.size()) + ":";
    blake3_hasher_update(&h, length.data(), length.size());
    blake3_hasher_update(&h, value.data(), value.size());
}
std::string point_id(const SemanticRecord& r) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    hash_field(h, r.signature_id);
    hash_field(h, r.identity.model_id);
    hash_field(h, r.identity.generation);
    unsigned char b[16];
    blake3_hasher_finalize(&h, b, 16);
    static constexpr char x[] = "0123456789abcdef";
    std::string out(36, '0');
    std::size_t nibble = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23)
            out[i] = '-';
        else {
            const auto byte = b[nibble / 2];
            out[i] = x[(nibble % 2 == 0) ? (byte >> 4) : (byte & 0x0f)];
            ++nibble;
        }
    }
    return out;
}
} // namespace
QdrantSemanticStore::QdrantSemanticStore(std::string e, std::string k, std::string c,
                                         std::uint32_t d)
    : endpoint_(std::move(e)), key_(std::move(k)), collection_(std::move(c)), dimension_(d) {
    if (endpoint_.empty() || key_.empty() || collection_.empty())
        throw std::invalid_argument("Qdrant config missing");
    curl_global_init(CURL_GLOBAL_DEFAULT);
}
void QdrantSemanticStore::upsert(const SemanticRecord& r) {
    validate_semantic_identity(r.identity, r.vector);
    if (r.identity.dimension != dimension_ || r.signature_id.empty())
        throw std::invalid_argument("Qdrant dimension/id mismatch");
    const auto id = point_id(r);
    call(endpoint_ + "/collections/" + collection_ + "/points?wait=true", key_, "PUT",
         {{"points", json::array({{{"id", id},
                                   {"vector", vec(r.vector)},
                                   {"payload",
                                    {{"signature_id", r.signature_id},
                                     {"repository_id", r.repository_id},
                                     {"index_epoch", r.index_epoch},
                                     {"model_id", r.identity.model_id},
                                     {"dimension", r.identity.dimension},
                                     {"metric", r.identity.metric},
                                     {"generation", r.identity.generation}}}}})}});
}
void QdrantSemanticStore::erase(const std::string& id, const std::string& g) {
    call(endpoint_ + "/collections/" + collection_ + "/points/delete?wait=true", key_, "POST",
         {{"filter",
           {{"must", json::array({{{"key", "generation"}, {"match", {{"value", g}}}},
                                  {{"key", "signature_id"}, {"match", {{"value", id}}}}})}}}});
}
std::vector<SemanticHit> QdrantSemanticStore::search(const std::vector<float>& v,
                                                     const SemanticIdentity& i,
                                                     std::size_t l) const {
    validate_semantic_identity(i, v);
    if (i.dimension != dimension_ || !l || l > 200)
        throw std::invalid_argument("Qdrant search bounds");
    json must = json::array();
    must.push_back({{"key", "model_id"}, {"match", {{"value", i.model_id}}}});
    must.push_back({{"key", "dimension"}, {"match", {{"value", i.dimension}}}});
    must.push_back({{"key", "metric"}, {"match", {{"value", i.metric}}}});
    must.push_back({{"key", "generation"}, {"match", {{"value", i.generation}}}});
    json body = {
        {"vector", vec(v)}, {"limit", l}, {"with_payload", true}, {"filter", {{"must", must}}}};
    const auto response = json::parse(
        call(endpoint_ + "/collections/" + collection_ + "/points/search", key_, "POST", body));
    std::vector<SemanticHit> out;
    for (const auto& p : response.at("result"))
        out.push_back({p.at("payload").at("signature_id"), p.at("score")});
    return out;
}
} // namespace axon::portfolio
