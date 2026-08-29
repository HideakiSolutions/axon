#include "pending_writes.hpp"

#include <chrono>
#include <fstream>
#include <string>
#include <unordered_set>

namespace axon {
namespace {

int read_attempts(const std::filesystem::path& path) {
    std::ifstream input(path);
    int attempts = 0;
    if (input >> attempts && attempts > 0) return attempts;
    return 0;
}

void write_attempts(const std::filesystem::path& path, int attempts) {
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return;
        output << attempts << '\n';
        output.flush();
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(temporary, path, ec);
    if (ec) std::filesystem::remove(temporary, ec);
}

std::filesystem::path quarantine_path(const std::filesystem::path& axon_dir) {
    const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    auto candidate = axon_dir / ("pending-writes.failed-" + std::to_string(epoch) + ".txt");
    for (int suffix = 1; std::filesystem::exists(candidate); ++suffix) {
        candidate = axon_dir / ("pending-writes.failed-" + std::to_string(epoch) + "-" +
                                std::to_string(suffix) + ".txt");
    }
    return candidate;
}

} // namespace

PendingWriteClaim PendingWriteClaim::acquire(const std::filesystem::path& axon_dir,
                                             int max_attempts) {
    PendingWriteClaim claim;
    claim.claim_path_ = axon_dir / "pending-writes.processing";
    claim.attempts_path_ = axon_dir / "pending-writes.processing.attempts";
    const auto queue_path = axon_dir / "pending-writes.txt";

    std::error_code ec;
    if (std::filesystem::exists(claim.claim_path_, ec)) {
        const int previous_attempts = read_attempts(claim.attempts_path_);
        if (previous_attempts >= max_attempts) {
            const auto failed = quarantine_path(axon_dir);
            std::filesystem::rename(claim.claim_path_, failed, ec);
            if (!ec) {
                claim.quarantined_path_ = failed;
                std::filesystem::remove(claim.attempts_path_, ec);
            }
        }
    }

    ec.clear();
    if (!std::filesystem::exists(claim.claim_path_, ec)) {
        ec.clear();
        if (!std::filesystem::exists(queue_path, ec) ||
            std::filesystem::file_size(queue_path, ec) == 0) {
            return claim;
        }
        ec.clear();
        std::filesystem::rename(queue_path, claim.claim_path_, ec);
        if (ec) return claim;
    }

    claim.attempt_ = read_attempts(claim.attempts_path_) + 1;
    write_attempts(claim.attempts_path_, claim.attempt_);

    std::ifstream input(claim.claim_path_);
    if (!input) return claim;
    std::unordered_set<std::string> seen;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || !seen.insert(line).second) continue;
        claim.paths_.emplace_back(line);
    }

    if (claim.paths_.empty()) claim.acknowledge();
    return claim;
}

void PendingWriteClaim::acknowledge() {
    std::error_code ec;
    std::filesystem::remove(claim_path_, ec);
    ec.clear();
    std::filesystem::remove(attempts_path_, ec);
    paths_.clear();
    attempt_ = 0;
}

} // namespace axon
