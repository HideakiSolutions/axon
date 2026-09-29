// Windows-native backend: one ReadDirectoryChangesW loop over the project
// root (recursive), serviced by a dedicated producer thread that keeps a
// read outstanding at all times. Batches are normalized into a mutex+condvar
// queue that wait_for_changes drains — the same shape as the FSEvents
// backend, which this mirrors deliberately (it is the one that already
// survived a lifecycle hardening pass, PR #70).
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "watcher.hpp"
#include "skip_dirs.hpp"
#include "../parser/parser.hpp"

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

class Win32Watcher : public Watcher {
public:
    Win32Watcher(const Config& cfg, std::string& error) : root_(cfg.project_root) {
        dir_ =
            CreateFileW(root_.c_str(), FILE_LIST_DIRECTORY,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (dir_ == INVALID_HANDLE_VALUE) {
            error = "CreateFileW(project root) failed, error " + std::to_string(GetLastError());
            return;
        }
        io_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!io_event_ || !stop_event_) {
            error = "CreateEventW failed, error " + std::to_string(GetLastError());
            return;
        }

        // Every ReadDirectoryChangesW — including the first — is issued from
        // the producer thread: an async I/O request is cancelled when its
        // issuing thread exits, so arming from the constructor would tie the
        // watch to the wrong lifetime (the FSEvents lesson, transposed).
        // The handshake keeps the init contract: when make_watcher returns,
        // a read is already outstanding and no pre-first-wait event is lost.
        std::promise<bool> armed_promise;
        auto armed = armed_promise.get_future();
        thread_ = std::thread([this, &armed_promise]() { run(armed_promise); });

        if (!armed.get()) {
            error = "ReadDirectoryChangesW failed to arm, error " + std::to_string(arm_error_);
            thread_.join();
            return;
        }
        ok_ = true;
    }

    ~Win32Watcher() override { teardown(); }

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

    const char* backend_name() const override { return "win32"; }

private:
    static constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
                                           FILE_NOTIFY_CHANGE_DIR_NAME |
                                           FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;

    bool pending_has_any() const {
        return !pending_.changed.empty() || pending_.deleted || pending_.overflow;
    }

    void teardown() {
        stop();
        if (thread_.joinable()) {
            SetEvent(stop_event_); // producer cancels its own outstanding I/O
            thread_.join();
        }
        if (dir_ != INVALID_HANDLE_VALUE) {
            CloseHandle(dir_);
            dir_ = INVALID_HANDLE_VALUE;
        }
        if (io_event_) {
            CloseHandle(io_event_);
            io_event_ = nullptr;
        }
        if (stop_event_) {
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
        }
    }

    bool issue_read() {
        ResetEvent(io_event_);
        ov_ = OVERLAPPED{};
        ov_.hEvent = io_event_;
        if (!ReadDirectoryChangesW(dir_, buf_, sizeof(buf_), TRUE /*bWatchSubtree*/, kNotifyFilter,
                                   nullptr, &ov_, nullptr)) {
            arm_error_ = GetLastError();
            return false;
        }
        return true;
    }

    void push(WatchEvent&& batch) {
        if (batch.changed.empty() && !batch.deleted && !batch.overflow) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.merge(batch);
        }
        cv_.notify_all();
    }

    // Copy the notification buffer into a WatchEvent. Runs before the
    // re-arm, because the next ReadDirectoryChangesW reuses buf_.
    WatchEvent parse_buffer(DWORD bytes) {
        WatchEvent batch;
        if (bytes == 0) {
            // Successful completion with zero bytes = the buffer was too
            // small for everything that happened; the OS dropped the batch.
            batch.overflow = true;
            return batch;
        }
        auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buf_);
        for (;;) {
            std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));
            fs::path rel(name); // relative to root, '\'-separated
            if (!rel.empty() && !path_has_skipped_component(rel)) {
                switch (info->Action) {
                case FILE_ACTION_ADDED:
                case FILE_ACTION_MODIFIED:
                case FILE_ACTION_RENAMED_NEW_NAME:
                    // The event does not say file-vs-directory; the language
                    // extension filter doubles as the discriminator (a new
                    // subdirectory has no indexable extension, and its files
                    // arrive as their own events thanks to bWatchSubtree).
                    if (is_indexable_file(rel)) batch.changed.emplace_back(rel);
                    break;
                case FILE_ACTION_REMOVED:
                case FILE_ACTION_RENAMED_OLD_NAME:
                    // Gone paths cannot be stat'ed to learn their type. An
                    // indexable extension means a tracked file; extensionless
                    // means "probably a directory that may have taken tracked
                    // files with it". Either way a prune sweep is idempotent —
                    // the bias is toward never missing a deletion.
                    if (is_indexable_file(rel) || !rel.has_extension()) batch.deleted = true;
                    break;
                default:
                    break;
                }
            }
            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<const char*>(info) + info->NextEntryOffset);
        }
        return batch;
    }

    void run(std::promise<bool>& armed_promise) {
        if (!issue_read()) {
            armed_promise.set_value(false);
            return;
        }
        armed_promise.set_value(true);

        HANDLE handles[2] = {stop_event_, io_event_};
        for (;;) {
            DWORD which = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (which == WAIT_OBJECT_0) { // stop: reap our outstanding read
                CancelIoEx(dir_, &ov_);
                DWORD ignored = 0;
                GetOverlappedResult(dir_, &ov_, &ignored, TRUE);
                return;
            }
            if (which != WAIT_OBJECT_0 + 1) return; // WAIT_FAILED — give up

            DWORD bytes = 0;
            if (GetOverlappedResult(dir_, &ov_, &bytes, FALSE)) {
                WatchEvent batch = parse_buffer(bytes);
                if (!issue_read()) {
                    // Handle went bad mid-flight (root deleted/renamed):
                    // surface what we have plus a prune signal, then go dark.
                    batch.deleted = true;
                    push(std::move(batch));
                    return;
                }
                push(std::move(batch));
                continue;
            }

            const DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED) return; // our own CancelIoEx
            if (err == ERROR_NOTIFY_ENUM_DIR) {
                // Same meaning as the zero-byte completion: too many changes,
                // caller must rescan.
                WatchEvent batch;
                batch.overflow = true;
                if (!issue_read()) {
                    batch.deleted = true;
                    push(std::move(batch));
                    return;
                }
                push(std::move(batch));
                continue;
            }
            // ERROR_ACCESS_DENIED / ERROR_INVALID_HANDLE and anything else
            // unexpected: the watch is unrecoverable (typically the root was
            // deleted or renamed under us). Signal a prune and exit; the
            // fallback story for a vanished root belongs to the caller.
            WatchEvent batch;
            batch.deleted = true;
            push(std::move(batch));
            return;
        }
    }

    fs::path root_;
    HANDLE dir_ = INVALID_HANDLE_VALUE;
    HANDLE io_event_ = nullptr;
    HANDLE stop_event_ = nullptr;
    OVERLAPPED ov_{};
    alignas(DWORD) char buf_[64 * 1024]{};
    DWORD arm_error_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    WatchEvent pending_;
    bool stopped_ = false;
    bool ok_ = false;
};

} // namespace

std::unique_ptr<Watcher> make_win32_watcher(const Config& cfg, std::string& error) {
    auto w = std::make_unique<Win32Watcher>(cfg, error);
    if (!w->ok()) return nullptr;
    return w;
}

} // namespace axon

#endif // _WIN32
