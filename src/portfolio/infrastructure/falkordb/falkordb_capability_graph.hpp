#pragma once

#include "portfolio/application/graph/capability_graph_projection.hpp"

#include <mutex>

struct redisContext;

namespace axon::portfolio {

class FalkorDbCapabilityGraph final : public CapabilityGraphProjection {
  public:
    FalkorDbCapabilityGraph(std::string host, int port, std::string graph_name);
    ~FalkorDbCapabilityGraph() override;

    void replace_repository(const RepositoryStreamKey& stream,
                            const std::string& generation,
                            const std::vector<CapabilitySignature>& signatures) override;
    GraphTraversal traverse(const RepositoryStreamKey& stream,
                            const std::string& signature_id,
                            std::size_t max_depth,
                            std::size_t max_nodes) const override;

  private:
    void execute(const std::string& query) const;

    std::string host_;
    std::string graph_name_;
    int port_;
    mutable redisContext* context_ = nullptr;
    mutable std::mutex mutex_;
};

} // namespace axon::portfolio
