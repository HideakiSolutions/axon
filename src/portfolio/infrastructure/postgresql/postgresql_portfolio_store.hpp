#pragma once

#include "portfolio/application/portfolio_store.hpp"

#include <libpq-fe.h>

#include <memory>
#include <mutex>
#include <string>

namespace axon::portfolio {

class PostgresqlPortfolioStore final : public PortfolioStore {
public:
    PostgresqlPortfolioStore(std::string connection_string, std::string schema,
                             bool drop_schema_on_destroy = false);
    ~PostgresqlPortfolioStore() override;
    PostgresqlPortfolioStore(const PostgresqlPortfolioStore&) = delete;
    PostgresqlPortfolioStore& operator=(const PostgresqlPortfolioStore&) = delete;

    ProviderCapabilities capabilities() const override;
    ProviderHealth health() const override;
    std::string schema_version() const override;
    std::string protocol_version() const override;
    ApplyResult apply(const RepositoryStreamKey&, std::uint64_t,
                      const std::vector<ProjectionEvent>&) override;
    ReplaceResult replace_repository_stream(const RepositorySnapshot&, std::uint64_t) override;
    ApplyResult reidentify_repository_stream(const RepositoryReidentification&,
                                             std::uint64_t) override;
    CursorEpochManifest stream_state(const RepositoryStreamKey&) const override;
    StreamProjection inspect_repository_stream(const RepositoryStreamKey&,
                                               std::size_t) const override;
    MaintenanceResult maintenance(MaintenanceKind) override;

    std::size_t pending_outbox_count() const;
    const std::string& schema() const { return schema_; }

private:
    void migrate();
    CursorEpochManifest state_unlocked(const RepositoryStreamKey&, bool lock_row = false) const;
    std::pair<RepositoryStreamKey, CursorEpochManifest>
    physical_state_unlocked(const std::string&, bool lock_row = false) const;
    std::string table(const char*) const;

    std::string schema_;
    bool drop_schema_on_destroy_ = false;
    bool schema_created_by_this_instance_ = false;
    PGconn* connection_ = nullptr;
    mutable std::mutex mutex_;
};

std::unique_ptr<PortfolioStore> make_postgresql_conformance_store();

} // namespace axon::portfolio
