#pragma once

#include <filesystem>
#include <optional>
#include <vector>

namespace axon {

// Durable claim over .axon/pending-writes.txt. The processing file remains on
// disk until acknowledge() so a process crash or indexing exception is
// replayed on the next drain. Replays are safe because index_files is
// idempotent and paths are deduplicated when the claim is read.
class PendingWriteClaim {
public:
    static PendingWriteClaim acquire(const std::filesystem::path& axon_dir, int max_attempts = 5);

    PendingWriteClaim() = default;
    PendingWriteClaim(const PendingWriteClaim&) = delete;
    PendingWriteClaim& operator=(const PendingWriteClaim&) = delete;
    PendingWriteClaim(PendingWriteClaim&&) noexcept = default;
    PendingWriteClaim& operator=(PendingWriteClaim&&) noexcept = default;

    bool has_batch() const { return !paths_.empty(); }
    const std::vector<std::filesystem::path>& paths() const { return paths_; }
    int attempt() const { return attempt_; }
    const std::optional<std::filesystem::path>& quarantined_path() const {
        return quarantined_path_;
    }

    void acknowledge();

private:
    std::filesystem::path claim_path_;
    std::filesystem::path attempts_path_;
    std::vector<std::filesystem::path> paths_;
    int attempt_ = 0;
    std::optional<std::filesystem::path> quarantined_path_;
};

} // namespace axon
