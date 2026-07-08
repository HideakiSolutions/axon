// Polling backend — the portable fallback (and the Windows default). This is
// the change-detection half of the pre-1.3 `axon watch` loop, extracted
// behind the Watcher interface with identical mtime+size semantics.
#include "watcher.hpp"
#include "skip_dirs.hpp"
#include "../parser/parser.hpp"
#include <atomic>
#include <thread>
#include <unordered_map>

namespace axon {
namespace fs = std::filesystem;

namespace {

struct Stamp {
    int64_t mtime = 0;
    uintmax_t size = 0;
};

class PollWatcher : public Watcher {
public:
    PollWatcher(const Config& cfg, int interval_ms)
        : root_(cfg.project_root), interval_ms_(interval_ms) {
        seen_ = snapshot();
    }

    bool wait_for_changes(std::chrono::milliseconds max_wait, WatchEvent& out) override {
        // Sleep in small slices so stop() takes effect promptly, then diff a
        // fresh snapshot against the baseline. max_wait==0 snapshots at once
        // (the post-debounce drain).
        auto remaining = max_wait;
        const auto slice = std::chrono::milliseconds(100);
        while (remaining.count() > 0 && !stopped_.load()) {
            auto nap = remaining < slice ? remaining : slice;
            std::this_thread::sleep_for(nap);
            remaining -= nap;
        }
        if (stopped_.load()) return false;

        auto next = snapshot();
        bool any = false;
        for (const auto& [path_key, st] : next) {
            auto it = seen_.find(path_key);
            if (it == seen_.end() || it->second.mtime != st.mtime || it->second.size != st.size) {
                out.changed.emplace_back(path_key);
                any = true;
            }
        }
        for (const auto& [path_key, st] : seen_) {
            if (!next.count(path_key)) {
                out.deleted = true;
                any = true;
            }
        }
        seen_ = std::move(next);
        return any;
    }

    void stop() override { stopped_.store(true); }

    const char* backend_name() const override { return "poll"; }

private:
    static Stamp stamp(const fs::path& p) {
        std::error_code ec;
        auto t = fs::last_write_time(p, ec);
        auto s = fs::file_size(p, ec);
        return Stamp{ec ? int64_t{0} : static_cast<int64_t>(t.time_since_epoch().count()),
                     ec ? uintmax_t{0} : s};
    }

    std::unordered_map<std::string, Stamp> snapshot() const {
        std::unordered_map<std::string, Stamp> out;
        for (auto it = fs::recursive_directory_iterator(
                 root_, fs::directory_options::skip_permission_denied);
             it != fs::end(it); ++it) {
            // Prune skip-dirs from the walk — the indexer would refuse these
            // files anyway, so detecting them only produced no-op reindexes
            // (and made node_modules churn wake the watcher).
            if (it->is_directory() && is_hard_skip_dir(it->path().filename().string())) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file()) continue;
            auto ext = it->path().extension().string();
            if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
            if (!language_from_extension(ext)) continue;
            std::error_code ec;
            auto rel = fs::relative(it->path(), root_, ec);
            if (!ec && !rel.empty() && rel.generic_string().rfind("..", 0) != 0)
                out[rel.generic_string()] = stamp(it->path());
        }
        return out;
    }

    fs::path root_;
    int interval_ms_;
    std::unordered_map<std::string, Stamp> seen_;
    std::atomic<bool> stopped_{false};
};

} // namespace

std::unique_ptr<Watcher> make_poll_watcher(const Config& cfg, int interval_ms) {
    return std::make_unique<PollWatcher>(cfg, interval_ms);
}

} // namespace axon
