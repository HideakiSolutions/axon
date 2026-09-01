#include "portfolio/infrastructure/postgresql/pgvector_semantic_store.hpp"
#include "portfolio/application/search/fail_soft_semantic_search.hpp"
#ifdef AXON_HAVE_QDRANT
#include "portfolio/infrastructure/qdrant/qdrant_semantic_store.hpp"
#endif
#include <gtest/gtest.h>
#include <cstdlib>
#include <stdexcept>

namespace {
std::string dsn() {
    const char* value = std::getenv("AXON_PORTFOLIO_SEMANTIC_DSN");
    return value ? value : "";
}
#ifdef AXON_HAVE_QDRANT
struct QdrantCleanup {
    axon::portfolio::QdrantSemanticStore& store;
    std::string id, generation;
    ~QdrantCleanup() {
        try {
            store.erase(id, generation);
        } catch (...) {
        }
    }
};
#endif
struct PgCleanup {
    axon::portfolio::PgvectorSemanticStore& store;
    std::vector<std::pair<std::string, std::string>> records;
    ~PgCleanup() {
        for (const auto& [id, generation] : records)
            try {
                store.erase(id, generation);
            } catch (...) {
            }
    }
};

class MemoryStore final : public axon::portfolio::SemanticCapabilityStore {
public:
    bool unavailable = false;
    std::vector<axon::portfolio::SemanticHit> hits;
    bool contains = false;
    void upsert(const axon::portfolio::SemanticRecord&) override {
        if (unavailable) throw std::runtime_error("unavailable");
        contains = true;
    }
    void erase(const std::string&, const std::string&) override {
        if (unavailable) throw std::runtime_error("unavailable");
        contains = false;
    }
    std::vector<axon::portfolio::SemanticHit> search(const std::vector<float>&,
                                                     const axon::portfolio::SemanticIdentity&,
                                                     std::size_t) const override {
        if (unavailable) throw std::runtime_error("unavailable");
        return contains ? hits : std::vector<axon::portfolio::SemanticHit>{};
    }
};
TEST(PgvectorSemanticStore, UpsertsFiltersAndDeletesByGeneration) {
    if (dsn().empty()) GTEST_SKIP() << "AXON_PORTFOLIO_SEMANTIC_DSN not configured";
    axon::portfolio::PgvectorSemanticStore store(dsn(), 3, "axon_semantic_capabilities_g9_test");
    PgCleanup cleanup{store,
                      {{"aaaaaaaaaaaaaaaa", "generation-a"},
                       {"bbbbbbbbbbbbbbbb", "generation-a"},
                       {"cccccccccccccccc", "generation-a"},
                       {"cccccccccccccccc", "generation-b"},
                       {"cccccccccccccccc", "generation-c"}}};
    const axon::portfolio::SemanticIdentity identity{"test-model", 3, "cosine", "generation-a"};
    store.upsert({"aaaaaaaaaaaaaaaa",
                  "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb",
                  "epoch-aaaaaaaaaaaa",
                  identity,
                  {1, 0, 0}});
    store.upsert({"bbbbbbbbbbbbbbbb",
                  "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb",
                  "epoch-bbbbbbbbbbbb",
                  identity,
                  {0, 1, 0}});
    const auto hits = store.search({1, 0, 0}, identity, 2);
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits.front().signature_id, "aaaaaaaaaaaaaaaa");
    store.erase("aaaaaaaaaaaaaaaa", "generation-a");
    EXPECT_EQ(store.search({1, 0, 0}, identity, 2).size(), 1u);
    store.erase("bbbbbbbbbbbbbbbb", "generation-a");
    const axon::portfolio::SemanticIdentity next{"test-model", 3, "cosine", "generation-b"};
    store.upsert({"cccccccccccccccc",
                  "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb",
                  "epoch-cccccccccccc",
                  identity,
                  {1, 0, 0}});
    store.upsert({"cccccccccccccccc",
                  "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb",
                  "epoch-dddddddddddd",
                  next,
                  {0, 1, 0}});
    EXPECT_EQ(store.search({1, 0, 0}, identity, 2).size(), 1u);
    EXPECT_EQ(store.search({0, 1, 0}, next, 2).size(), 1u);
    const axon::portfolio::SemanticIdentity next_model{"test-model-2", 3, "cosine", "generation-c"};
    store.upsert({"cccccccccccccccc",
                  "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb",
                  "epoch-eeeeeeeeeeee",
                  next_model,
                  {0, 0, 1}});
    EXPECT_EQ(store.search({0, 0, 1}, next_model, 2).size(), 1u);
}
TEST(FailSoftSemanticSearch, FallsBackAndExposesDegradedMode) {
    MemoryStore primary, fallback;
    fallback.hits = {{"fallback-signature", 0.9F}};
    fallback.contains = true;
    primary.unavailable = true;
    axon::portfolio::FailSoftSemanticSearch search(primary, fallback);
    const axon::portfolio::SemanticIdentity identity{"test-model", 3, "cosine", "generation-a"};
    const auto result = search.search({1, 0, 0}, identity, 1);
    EXPECT_EQ(result.mode, axon::portfolio::SemanticSearchMode::fallback);
    ASSERT_EQ(result.hits.size(), 1u);
    EXPECT_EQ(result.hits.front().signature_id, "fallback-signature");
    EXPECT_FALSE(result.diagnostic.empty());
}
TEST(FailSoftSemanticSearch, KeepsFallbackWarmAcrossOutageAndDelete) {
    MemoryStore primary, fallback;
    fallback.hits = {{"signature", 1.0F}};
    axon::portfolio::FailSoftSemanticSearch search(primary, fallback);
    const axon::portfolio::SemanticIdentity identity{"test-model", 3, "cosine", "generation-a"};
    const axon::portfolio::SemanticRecord record{
        "signature", "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb", "epoch", identity, {1, 0, 0}};
    search.upsert(record);
    EXPECT_TRUE(primary.contains);
    EXPECT_TRUE(fallback.contains);
    primary.unavailable = true;
    const auto result = search.search(record.vector, identity, 1);
    EXPECT_EQ(result.mode, axon::portfolio::SemanticSearchMode::fallback);
    ASSERT_EQ(result.hits.size(), 1u);
    search.erase(record.signature_id, identity.generation);
    EXPECT_FALSE(fallback.contains);
    primary.unavailable = false;
    const auto after_recovery = search.search(record.vector, identity, 1);
    EXPECT_EQ(after_recovery.mode, axon::portfolio::SemanticSearchMode::fallback);
    EXPECT_TRUE(after_recovery.hits.empty());
    EXPECT_TRUE(search.primary_dirty());
}
TEST(FailSoftSemanticSearch, RetainsFallbackAfterFailedUpsertUntilReconcile) {
    MemoryStore primary, fallback;
    fallback.hits = {{"signature", 1.0F}};
    axon::portfolio::FailSoftSemanticSearch search(primary, fallback);
    const axon::portfolio::SemanticIdentity identity{"test-model", 3, "cosine", "generation-a"};
    const axon::portfolio::SemanticRecord record{
        "signature", "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb", "epoch", identity, {1, 0, 0}};
    primary.unavailable = true;
    search.upsert(record);
    primary.unavailable = false;
    const auto after_recovery = search.search(record.vector, identity, 1);
    EXPECT_EQ(after_recovery.mode, axon::portfolio::SemanticSearchMode::fallback);
    ASSERT_EQ(after_recovery.hits.size(), 1u);
    search.mark_primary_reconciled();
    EXPECT_FALSE(search.primary_dirty());
}
#ifdef AXON_HAVE_QDRANT
TEST(QdrantSemanticStore, UpsertsSearchesAndDeletesByGeneration) {
    const char* key = std::getenv("AXON_QDRANT_API_KEY");
    if (!key || !*key) GTEST_SKIP() << "AXON_QDRANT_API_KEY not configured";
    axon::portfolio::QdrantSemanticStore store("http://127.0.0.1:6333", key,
                                               "axon-portfolio-capabilities-v1", 768);
    const axon::portfolio::SemanticIdentity identity{"test-model", 768, "cosine",
                                                     "generation-qdrant"};
    std::vector<float> vector(768);
    vector[0] = 1;
    store.upsert({"11111111111111111111111111111111", "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb",
                  "epoch-111111111111", identity, vector});
    QdrantCleanup cleanup{store, "11111111111111111111111111111111", "generation-qdrant"};
    std::vector<float> updated(768);
    updated[1] = 1;
    store.upsert({"11111111111111111111111111111111", "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb",
                  "epoch-222222222222", identity, updated});
    const auto hits = store.search(vector, identity, 2);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits.front().signature_id, "11111111111111111111111111111111");
    store.erase("11111111111111111111111111111111", "generation-qdrant");
}
#endif
} // namespace
