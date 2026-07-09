#include "watcher.hpp"
#include <cstdlib>
#include <iostream>

namespace axon {

void WatchEvent::merge(const WatchEvent& other) {
    changed.insert(changed.end(), other.changed.begin(), other.changed.end());
    deleted = deleted || other.deleted;
    overflow = overflow || other.overflow;
}

// Provided by watcher_poll.cpp (all platforms).
std::unique_ptr<Watcher> make_poll_watcher(const Config& cfg, int interval_ms);

// Provided by the platform TU; the other platforms compile it as an empty
// translation unit, so the symbol is only referenced where it exists.
#if defined(__linux__)
std::unique_ptr<Watcher> make_inotify_watcher(const Config& cfg, std::string& error);
#elif defined(__APPLE__)
std::unique_ptr<Watcher> make_fsevents_watcher(const Config& cfg, std::string& error);
#elif defined(_WIN32)
std::unique_ptr<Watcher> make_win32_watcher(const Config& cfg, std::string& error);
#endif

namespace {
// Test seam: flags the first detected change batch as a kernel-queue
// overflow so the watch loop's full-rescan path can be exercised
// hermetically (a real IN_Q_OVERFLOW needs unreproducible kernel load).
class ForcedOverflowOnce : public Watcher {
public:
    explicit ForcedOverflowOnce(std::unique_ptr<Watcher> inner) : inner_(std::move(inner)) {}
    bool wait_for_changes(std::chrono::milliseconds max_wait, WatchEvent& out) override {
        bool any = inner_->wait_for_changes(max_wait, out);
        if (any && !fired_) {
            out.overflow = true;
            fired_ = true;
        }
        return any;
    }
    void stop() override { inner_->stop(); }
    const char* backend_name() const override { return inner_->backend_name(); }

private:
    std::unique_ptr<Watcher> inner_;
    bool fired_ = false;
};

bool env_truthy(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::string(v) != "0";
}
} // namespace

std::unique_ptr<Watcher> make_watcher(const Config& cfg, WatchBackend pref, int poll_interval_ms,
                                      std::string& chosen) {
    if (env_truthy("AXON_WATCH_FORCE_POLL")) pref = WatchBackend::Poll;

    auto finish = [](std::unique_ptr<Watcher> w) -> std::unique_ptr<Watcher> {
        if (w && env_truthy("AXON_WATCH_FORCE_OVERFLOW"))
            return std::make_unique<ForcedOverflowOnce>(std::move(w));
        return w;
    };

    if (pref != WatchBackend::Poll) {
        std::string error;
        std::unique_ptr<Watcher> native;
        if (env_truthy("AXON_WATCH_FORCE_NATIVE_FAIL")) {
            error = "forced failure (AXON_WATCH_FORCE_NATIVE_FAIL)";
        } else {
#if defined(__linux__)
            native = make_inotify_watcher(cfg, error);
#elif defined(__APPLE__)
            native = make_fsevents_watcher(cfg, error);
#elif defined(_WIN32)
            native = make_win32_watcher(cfg, error);
#else
            error = "no native backend on this platform";
#endif
        }
        if (native) {
            chosen = native->backend_name();
            return finish(std::move(native));
        }
        if (pref == WatchBackend::Native) {
            std::cerr << "[axon] native watcher init failed (" << error << ")\n";
            return nullptr;
        }
        std::cerr << "[axon] native watcher init failed (" << error
                  << "); falling back to polling\n";
    }

    chosen = "poll";
    return finish(make_poll_watcher(cfg, poll_interval_ms));
}

} // namespace axon
