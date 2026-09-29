// macOS-native backend: one FSEventStream over the project root with
// file-level events, serviced by a dedicated CFRunLoop thread. The callback
// normalizes events into a mutex+condvar queue that wait_for_changes drains —
// the Watcher interface stays synchronous like the other backends.
#if defined(__APPLE__)

#include "watcher.hpp"
#include "skip_dirs.hpp"
#include "../parser/parser.hpp"

#include <CoreServices/CoreServices.h>

#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace axon {
namespace fs = std::filesystem;

namespace {

bool is_indexable_file(const fs::path& p) {
    auto ext = p.extension().string();
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    return language_from_extension(ext).has_value();
}

bool path_has_skipped_component(const fs::path& rel) {
    for (const auto& part : rel)
        if (is_hard_skip_dir(part.string())) return true;
    return false;
}

class FseventsWatcher : public Watcher {
public:
    FseventsWatcher(const Config& cfg, std::string& error) : root_(cfg.project_root) {
        CFStringRef cf_path =
            CFStringCreateWithCString(nullptr, root_.c_str(), kCFStringEncodingUTF8);
        CFArrayRef paths = CFArrayCreate(nullptr, (const void**)&cf_path, 1, nullptr);

        FSEventStreamContext ctx{0, this, nullptr, nullptr, nullptr};
        stream_ = FSEventStreamCreate(
            nullptr, &FseventsWatcher::callback, &ctx, paths, kFSEventStreamEventIdSinceNow,
            /*latency=*/0.2,
            kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                kFSEventStreamCreateFlagWatchRoot);
        CFRelease(paths);
        CFRelease(cf_path);
        if (!stream_) {
            error = "FSEventStreamCreate failed";
            return;
        }

        // Schedule AND start the stream from inside the run-loop thread,
        // before CFRunLoopRun. A loop with no sources returns immediately —
        // scheduling from the outside raced that exit, leaving the stream on
        // a dead loop and CFRunLoopStop dereferencing a freed CFRunLoopRef
        // (segfaulted the whole test binary on macOS CI). The loop ref is
        // retained because we use it from other threads after the race.
        std::promise<bool> started_promise;
        auto started = started_promise.get_future();
        runloop_thread_ = std::thread([this, &started_promise]() {
            runloop_ = CFRunLoopGetCurrent();
            CFRetain(runloop_);
            FSEventStreamScheduleWithRunLoop(stream_, runloop_, kCFRunLoopDefaultMode);
            if (!FSEventStreamStart(stream_)) {
                FSEventStreamInvalidate(stream_); // also unschedules
                FSEventStreamRelease(stream_);
                stream_ = nullptr;
                started_promise.set_value(false);
                return; // never entered CFRunLoopRun
            }
            started_promise.set_value(true);
            CFRunLoopRun(); // the stream keeps the loop alive until stopped
        });

        if (!started.get()) {
            error = "FSEventStreamStart failed";
            runloop_thread_.join();
            if (runloop_) {
                CFRelease(runloop_);
                runloop_ = nullptr;
            }
            return;
        }
        ok_ = true;
    }

    ~FseventsWatcher() override { teardown(); }

    bool ok() const { return ok_; }

    bool wait_for_changes(std::chrono::milliseconds max_wait, WatchEvent& out) override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!stopped_ && !pending_has_any() && max_wait.count() > 0)
            cv_.wait_for(lock, max_wait, [this] { return stopped_ || pending_has_any(); });
        if (stopped_) return false;
        if (!pending_has_any()) return false;
        out.merge(pending_);
        pending_ = WatchEvent{};
        return true;
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

    const char* backend_name() const override { return "fsevents"; }

private:
    bool pending_has_any() const {
        return !pending_.changed.empty() || pending_.deleted || pending_.overflow;
    }

    void teardown() {
        stop();
        if (stream_) {
            FSEventStreamStop(stream_);
            FSEventStreamInvalidate(stream_);
            FSEventStreamRelease(stream_);
            stream_ = nullptr;
        }
        if (runloop_) CFRunLoopStop(runloop_); // wakes CFRunLoopRun → thread exits
        if (runloop_thread_.joinable()) runloop_thread_.join();
        if (runloop_) {
            CFRelease(runloop_);
            runloop_ = nullptr;
        }
    }

    static void callback(ConstFSEventStreamRef, void* info, size_t num_events, void* event_paths,
                         const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
        auto* self = static_cast<FseventsWatcher*>(info);
        auto** paths = static_cast<char**>(event_paths);

        WatchEvent batch;
        for (size_t i = 0; i < num_events; i++) {
            const auto f = flags[i];
            if (f & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
                     kFSEventStreamEventFlagKernelDropped)) {
                batch.overflow = true;
                continue;
            }
            fs::path abs(paths[i]);
            std::error_code ec;
            auto rel = fs::relative(abs, self->root_, ec);
            if (ec || rel.empty() || rel.generic_string().rfind("..", 0) == 0) continue;
            if (path_has_skipped_component(rel)) continue;

            if (f & kFSEventStreamEventFlagItemIsDir) {
                // A directory removed/renamed away may take tracked files
                // with it; existence disambiguates rename-in vs rename-out.
                if ((f &
                     (kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemRenamed)) &&
                    !fs::exists(abs, ec))
                    batch.deleted = true;
                continue;
            }
            if (!is_indexable_file(abs)) continue;

            const bool exists = fs::exists(abs, ec) && !ec;
            if ((f & (kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemModified |
                      kFSEventStreamEventFlagItemRenamed)) &&
                exists)
                batch.changed.emplace_back(rel.generic_string());
            if ((f & (kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemRenamed)) &&
                !exists)
                batch.deleted = true;
        }

        if (!batch.changed.empty() || batch.deleted || batch.overflow) {
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->pending_.merge(batch);
            }
            self->cv_.notify_all();
        }
    }

    fs::path root_;
    FSEventStreamRef stream_ = nullptr;
    CFRunLoopRef runloop_ = nullptr;
    std::thread runloop_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    WatchEvent pending_;
    bool stopped_ = false;
    bool ok_ = false;
};

} // namespace

std::unique_ptr<Watcher> make_fsevents_watcher(const Config& cfg, std::string& error) {
    auto w = std::make_unique<FseventsWatcher>(cfg, error);
    if (!w->ok()) return nullptr;
    return w;
}

} // namespace axon

#endif // __APPLE__
