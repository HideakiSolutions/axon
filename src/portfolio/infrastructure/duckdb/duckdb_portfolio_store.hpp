#pragma once

#include "portfolio/application/portfolio_store.hpp"

#include <duckdb.hpp>
#include <filesystem>
#include <memory>

namespace axon::portfolio {

class DuckdbPortfolioStore final : public PortfolioStore {
public:
    DuckdbPortfolioStore();
    explicit DuckdbPortfolioStore(const std::filesystem::path& path);

    ProviderCapabilities capabilities() const override;
    ProviderHealth health() const override;
    std::string schema_version() const override;
    std::string protocol_version() const override;
    ApplyResult apply(const RepositoryStreamKey& stream, std::uint64_t expected_cursor,
                      const std::vector<ProjectionEvent>& events) override;
    ReplaceResult replace_repository_stream(const RepositorySnapshot& snapshot,
                                            std::uint64_t expected_cursor) override;
    ApplyResult reidentify_repository_stream(const RepositoryReidentification& reidentification,
                                             std::uint64_t expected_cursor) override;
    CursorEpochManifest stream_state(const RepositoryStreamKey& stream) const override;
    StreamProjection inspect_repository_stream(const RepositoryStreamKey& stream,
                                               std::size_t max_entities) const override;
    MaintenanceResult maintenance(MaintenanceKind kind) override;

private:
    void migrate();
    CursorEpochManifest state_unlocked(const RepositoryStreamKey& stream) const;
    std::pair<RepositoryStreamKey, CursorEpochManifest>
    physical_state_unlocked(const std::string& index_stream_id) const;

    std::shared_ptr<duckdb::DuckDB> database_;
    mutable std::unique_ptr<duckdb::Connection> connection_;
};

} // namespace axon::portfolio
