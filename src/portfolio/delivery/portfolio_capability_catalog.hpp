#pragma once

#include "portfolio/application/candidates/capability_candidates.hpp"
#include "portfolio/application/declarations/capability_declarations.hpp"
#include "portfolio/domain/capability_signature.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace axon::portfolio {

struct PortfolioSyncItem {
    std::string repository_id, index_stream_id, status, detail;
    std::size_t signatures = 0;
};
struct PortfolioSyncReport {
    std::vector<PortfolioSyncItem> repositories;
    bool degraded = false;
};

// Local, rebuildable query projection. It stores metadata signatures only: source bodies stay in
// each registered project index, which is opened read-only while synchronizing.
class PortfolioCapabilityCatalog {
public:
    explicit PortfolioCapabilityCatalog(std::filesystem::path path = {});
    PortfolioSyncReport sync(const std::optional<std::string>& group = std::nullopt,
                             bool force_rebuild = false);
    PortfolioSyncReport status() const;
    std::vector<CapabilitySignature>
    list(const std::optional<std::string>& repository_id = std::nullopt,
         std::size_t limit = 200) const;
    std::vector<CapabilitySignature> search(const std::string& query, std::size_t limit = 50) const;
    std::vector<CapabilityCandidate> duplicates(double threshold = 0.0,
                                                std::size_t limit = 100) const;
    DeclarationComparison drift(const std::filesystem::path& graph_root,
                                const std::filesystem::path& fragment,
                                std::size_t limit = 200) const;
    std::filesystem::path path() const;

private:
    std::filesystem::path path_;
};
} // namespace axon::portfolio
