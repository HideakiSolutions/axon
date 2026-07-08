// Linux-native backend: inotify over a SKIP_DIRS-pruned recursive watch-set.
// inotify is not recursive by itself, so every eligible directory gets its own
// watch descriptor and directories created mid-run are added (and re-scanned:
// files may land in them before the watch is armed). IN_CLOSE_WRITE instead of
// IN_MODIFY keeps it to one event per save.
#if defined(__linux__)

#include "watcher.hpp"
#include "skip_dirs.hpp"
#include "../parser/parser.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <unordered_map>

namespace axon {
namespace fs = std::filesystem;

namespace {

constexpr uint32_t DIR_MASK = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE |
                              IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;

bool is_indexable_file(const fs::path& p) {
    auto ext = p.extension().string();
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    return language_from_extension(ext).has_value();
}

class InotifyWatcher : public Watcher {
public:
    InotifyWatcher(const Config& cfg, std::string& error) : root_(cfg.project_root) {
        inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd_ < 0) {
            error = std::string("inotify_init1: ") + std::strerror(errno);
            return;
        }
        int pipefd[2];
        if (pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) != 0) {
            error = std::string("pipe2: ") + std::strerror(errno);
            close(inotify_fd_);
            inotify_fd_ = -1;
            return;
        }
        stop_read_fd_ = pipefd[0];
        stop_write_fd_ = pipefd[1];

        WatchEvent ignored;
        if (!add_dir_recursive(root_, ignored)) {
            error = init_error_.empty() ? "failed to arm directory watches" : init_error_;
            cleanup();
            return;
        }
        ok_ = true;
    }

    ~InotifyWatcher() override { cleanup(); }

    bool ok() const { return ok_; }

    bool wait_for_changes(std::chrono::milliseconds max_wait, WatchEvent& out) override {
        if (stopped_.load() || inotify_fd_ < 0) return false;

        bool any = drain(out); // events queued since the last call
        if (!any && max_wait.count() > 0) {
            pollfd fds[2] = {{inotify_fd_, POLLIN, 0}, {stop_read_fd_, POLLIN, 0}};
            int rc = ::poll(fds, 2, (int)max_wait.count());
            if (stopped_.load()) return false;
            if (rc > 0 && (fds[0].revents & POLLIN)) any = drain(out);
        }
        return any;
    }

    void stop() override {
        stopped_.store(true);
        if (stop_write_fd_ >= 0) {
            char b = 'x';
            [[maybe_unused]] ssize_t n = write(stop_write_fd_, &b, 1);
        }
    }

    const char* backend_name() const override { return "inotify"; }

private:
    void cleanup() {
        if (inotify_fd_ >= 0) close(inotify_fd_);
        if (stop_read_fd_ >= 0) close(stop_read_fd_);
        if (stop_write_fd_ >= 0) close(stop_write_fd_);
        inotify_fd_ = stop_read_fd_ = stop_write_fd_ = -1;
    }

    // Arm `dir` and every eligible descendant. When `emit` is given files
    // already present are reported as changed — covers `mkdir d && touch
    // d/x.ts` racing the watch registration.
    bool add_dir_recursive(const fs::path& dir, WatchEvent& emit, bool emit_existing = false) {
        int wd = inotify_add_watch(inotify_fd_, dir.c_str(), DIR_MASK);
        if (wd < 0) {
            if (errno == ENOSPC) {
                // Watch limit hit mid-run: flag overflow so the loop rescans.
                emit.overflow = true;
                return true;
            }
            init_error_ =
                std::string("inotify_add_watch(") + dir.string() + "): " + std::strerror(errno);
            return false;
        }
        wd_to_dir_[wd] = dir;

        std::error_code ec;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::end(it); it.increment(ec)) {
            if (it->is_directory(ec)) {
                auto name = it->path().filename().string();
                if (is_hard_skip_dir(name)) continue;
                if (it->is_symlink(ec)) continue; // don't follow symlinks (cycles)
                if (!add_dir_recursive(it->path(), emit, emit_existing)) return false;
            } else if (emit_existing && it->is_regular_file(ec) && is_indexable_file(it->path())) {
                push_changed(it->path(), emit);
            }
        }
        return true;
    }

    void push_changed(const fs::path& abs, WatchEvent& out) {
        std::error_code ec;
        auto rel = fs::relative(abs, root_, ec);
        if (!ec && !rel.empty() && rel.generic_string().rfind("..", 0) != 0)
            out.changed.emplace_back(rel.generic_string());
    }

    // Read until EAGAIN, translating raw inotify records into a WatchEvent.
    bool drain(WatchEvent& out) {
        bool any = false;
        alignas(inotify_event) char buf[16 * 1024];
        while (true) {
            ssize_t len = read(inotify_fd_, buf, sizeof(buf));
            if (len <= 0) break;
            for (ssize_t off = 0; off < len;) {
                auto* ev = reinterpret_cast<inotify_event*>(buf + off);
                off += sizeof(inotify_event) + ev->len;

                if (ev->mask & IN_Q_OVERFLOW) {
                    out.overflow = true;
                    any = true;
                    continue;
                }
                if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) {
                    // A watched directory itself went away.
                    if (wd_to_dir_.erase(ev->wd)) {
                        out.deleted = true;
                        any = true;
                    }
                    continue;
                }
                auto dir_it = wd_to_dir_.find(ev->wd);
                if (dir_it == wd_to_dir_.end() || ev->len == 0) continue;
                fs::path abs = dir_it->second / ev->name;

                if (ev->mask & IN_ISDIR) {
                    if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
                        if (is_hard_skip_dir(abs.filename().string())) continue;
                        // Arm the new tree and surface files that beat the watch.
                        add_dir_recursive(abs, out, /*emit_existing=*/true);
                        if (!out.changed.empty() || out.overflow) any = true;
                    } else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
                        out.deleted = true;
                        any = true;
                    }
                    continue;
                }

                if (!is_indexable_file(abs)) continue;
                if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) {
                    push_changed(abs, out);
                    any = true;
                } else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
                    out.deleted = true;
                    any = true;
                }
            }
        }
        return any;
    }

    fs::path root_;
    int inotify_fd_ = -1;
    int stop_read_fd_ = -1;
    int stop_write_fd_ = -1;
    bool ok_ = false;
    std::string init_error_;
    std::unordered_map<int, fs::path> wd_to_dir_;
    std::atomic<bool> stopped_{false};
};

} // namespace

std::unique_ptr<Watcher> make_inotify_watcher(const Config& cfg, std::string& error) {
    auto w = std::make_unique<InotifyWatcher>(cfg, error);
    if (!w->ok()) return nullptr;
    return w;
}

} // namespace axon

#endif // __linux__
