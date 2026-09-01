#include "fail_soft_semantic_search.hpp"

#include <exception>

namespace axon::portfolio {
FailSoftSemanticSearch::FailSoftSemanticSearch(SemanticCapabilityStore& primary,
                                               SemanticCapabilityStore& fallback)
    : primary_(primary), fallback_(fallback) {
}

SemanticSearchResult FailSoftSemanticSearch::search(const std::vector<float>& vector,
                                                    const SemanticIdentity& identity,
                                                    std::size_t limit) const {
    if (primary_dirty_.load())
        return {fallback_.search(vector, identity, limit), SemanticSearchMode::fallback,
                "primary semantic provider awaits reconciliation"};
    try {
        return {primary_.search(vector, identity, limit), SemanticSearchMode::primary, {}};
    } catch (const std::exception& error) {
        primary_dirty_.store(true);
        return {fallback_.search(vector, identity, limit), SemanticSearchMode::fallback,
                error.what()};
    }
}

void FailSoftSemanticSearch::upsert(const SemanticRecord& record) {
    // PostgreSQL is the durable fallback. Keep it current before attempting the
    // optional remote accelerator so a later Qdrant outage cannot expose stale data.
    fallback_.upsert(record);
    try {
        primary_.upsert(record);
    } catch (const std::exception&) {
        primary_dirty_.store(true);
    }
}

void FailSoftSemanticSearch::erase(const std::string& signature_id, const std::string& generation) {
    fallback_.erase(signature_id, generation);
    try {
        primary_.erase(signature_id, generation);
    } catch (const std::exception&) {
        primary_dirty_.store(true);
    }
}

void FailSoftSemanticSearch::mark_primary_reconciled() {
    primary_dirty_.store(false);
}
bool FailSoftSemanticSearch::primary_dirty() const {
    return primary_dirty_.load();
}
} // namespace axon::portfolio
