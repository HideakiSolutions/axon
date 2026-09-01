#pragma once

#include "portfolio/application/search/semantic_capability_store.hpp"

#include <libpq-fe.h>
#include <memory>
#include <mutex>

namespace axon::portfolio {
class PgvectorSemanticStore final : public SemanticCapabilityStore {
public:
    explicit PgvectorSemanticStore(std::string connection_string, std::uint32_t dimension = 768,
                                   std::string table_name = "axon_semantic_capabilities");
    ~PgvectorSemanticStore() override;
    void upsert(const SemanticRecord&) override;
    void erase(const std::string&, const std::string&) override;
    std::vector<SemanticHit> search(const std::vector<float>&, const SemanticIdentity&, std::size_t) const override;
private:
    void migrate();
    std::uint32_t dimension_;
    std::string table_name_;
    PGconn* connection_ = nullptr;
    mutable std::mutex mutex_;
};
} // namespace axon::portfolio
