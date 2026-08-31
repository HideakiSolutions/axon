#pragma once

#include "portfolio/application/portfolio_store.hpp"

#include <cstddef>
#include <filesystem>

namespace axon::portfolio {

struct RepositoryProjectionResult {
    RepositoryStreamKey stream;
    std::uint64_t cursor_before = 0;
    std::uint64_t cursor_after = 0;
    std::size_t events_applied = 0;
    bool rebuilt = false;
    bool stale = false;
};

// Pulls an authoritative project journal into a derived PortfolioStore. The source is always
// opened read-only and is never used to persist acknowledgements or projector cursors.
class DuckdbRepositoryProjector {
public:
    explicit DuckdbRepositoryProjector(PortfolioStore& store) : store_(store) {}

    RepositoryProjectionResult sync(const std::filesystem::path& registered_root,
                                    const std::filesystem::path& index_path,
                                    std::size_t batch_size = 500);
    RepositoryProjectionResult rebuild(const std::filesystem::path& registered_root,
                                       const std::filesystem::path& index_path);
    bool mark_stale(const RepositoryStreamKey& stream);

private:
    PortfolioStore& store_;
};

} // namespace axon::portfolio
