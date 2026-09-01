#pragma once
#include "portfolio/application/search/semantic_capability_store.hpp"
#include <string>
namespace axon::portfolio {
class QdrantSemanticStore final : public SemanticCapabilityStore {
public:
 QdrantSemanticStore(std::string endpoint,std::string api_key,std::string collection,std::uint32_t dimension=768);
 void upsert(const SemanticRecord&) override; void erase(const std::string&,const std::string&) override;
 std::vector<SemanticHit> search(const std::vector<float>&,const SemanticIdentity&,std::size_t) const override;
private: std::string endpoint_,key_,collection_; std::uint32_t dimension_;
}; }
