#pragma once

#include "portfolio/application/search/semantic_capability_store.hpp"

#include <atomic>
#include <string>

namespace axon::portfolio {

enum class SemanticSearchMode { primary, fallback };

struct SemanticSearchResult {
    std::vector<SemanticHit> hits;
    SemanticSearchMode mode = SemanticSearchMode::primary;
    std::string diagnostic;
};

class FailSoftSemanticSearch final {
public:
    FailSoftSemanticSearch(SemanticCapabilityStore& primary, SemanticCapabilityStore& fallback);
    SemanticSearchResult search(const std::vector<float>&, const SemanticIdentity&,
                                std::size_t) const;
    void upsert(const SemanticRecord&);
    void erase(const std::string& signature_id, const std::string& generation);
    // Call only after a successful replay/rebuild has made the primary current.
    void mark_primary_reconciled();
    bool primary_dirty() const;

private:
    SemanticCapabilityStore& primary_;
    SemanticCapabilityStore& fallback_;
    mutable std::atomic_bool primary_dirty_{false};
};
} // namespace axon::portfolio
