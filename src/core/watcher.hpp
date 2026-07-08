#pragma once
#include "config.hpp"
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace axon {

// One batch of filesystem changes, already filtered to indexable files
// (language extensions, hard-skip dirs pruned) and expressed as paths
// relative to the project root — the same contract index_files consumes.
struct WatchEvent {
    std::vector<std::filesystem::path> changed; // created or modified
    bool deleted = false;                       // something tracked disappeared → prune sweep
    bool overflow = false;                      // kernel queue overflowed → caller full-rescans
    void merge(const WatchEvent& other);
};

enum class WatchBackend { Auto, Native, Poll };

// Change-detection is the ONLY thing a backend implements. Debounce,
// incremental reindex, prune and telemetry live in the single watch loop
// (main.cpp), so the poll fallback is observationally identical to the
// native backends by construction.
class Watcher {
public:
    virtual ~Watcher() = default;

    // Block up to max_wait for changes; drain and coalesce whatever is
    // available into `out`. Returns true if anything changed. max_wait of
    // zero means "non-blocking drain" (used after the debounce window).
    virtual bool wait_for_changes(std::chrono::milliseconds max_wait, WatchEvent& out) = 0;

    // Wake a blocked wait_for_changes and make every later call return false.
    virtual void stop() = 0;

    virtual const char* backend_name() const = 0; // "inotify" | "fsevents" | "poll"
};

// Auto: try the platform's native backend, fall back to polling with a
// [axon] warning. Native: fail (nullptr + stderr) when the native backend
// can't initialize — diagnostic/test mode. Poll: always poll.
// AXON_WATCH_FORCE_POLL=1 forces Poll; AXON_WATCH_FORCE_NATIVE_FAIL=1 makes
// native init fail (exercises the fallback path in tests).
// `chosen` receives the effective backend name.
std::unique_ptr<Watcher> make_watcher(const Config& cfg, WatchBackend pref, int poll_interval_ms,
                                      std::string& chosen);

} // namespace axon
