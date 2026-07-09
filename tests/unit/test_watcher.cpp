#include <gtest/gtest.h>
#include "core/watcher.hpp"
#include "test_support.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ── Fixture: isolated project dir; every test builds its own Watcher. ────────

static fs::path make_temp_project() {
    static int counter = 0;
    auto p = fs::temp_directory_path() /
             ("axon_watcher_test_" + std::to_string(testsupport::pid()) + "_" +
              std::to_string(++counter));
    fs::create_directories(p / "src");
    std::ofstream(p / "src/seed.ts") << "export const seed = 1;\n";
    return p;
}

class WatcherTest : public ::testing::TestWithParam<axon::WatchBackend> {
protected:
    fs::path proj;
    axon::Config cfg;
    std::unique_ptr<axon::Watcher> watcher;
    std::string backend;

    void SetUp() override {
        proj = make_temp_project();
        cfg.project_root = proj;
        watcher = axon::make_watcher(cfg, GetParam(), /*poll_interval_ms=*/200, backend);
        if (GetParam() == axon::WatchBackend::Native && !watcher)
            GTEST_SKIP() << "native watcher unavailable in this sandbox";
        ASSERT_TRUE(watcher);
    }

    void TearDown() override {
        if (watcher) watcher->stop();
        watcher.reset();
        fs::remove_all(proj);
    }

    // Poll wait_for_changes until `pred` is satisfied or `budget` elapses.
    // Generous budgets keep CI runners with slow filesystems flake-free.
    bool wait_until(std::chrono::milliseconds budget,
                    const std::function<bool(const axon::WatchEvent&)>& pred,
                    axon::WatchEvent& acc) {
        auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            axon::WatchEvent ev;
            if (watcher->wait_for_changes(500ms, ev)) acc.merge(ev);
            if (pred(acc)) return true;
        }
        return false;
    }

    static bool has_changed(const axon::WatchEvent& ev, const std::string& rel) {
        for (const auto& p : ev.changed)
            if (p.generic_string() == rel) return true;
        return false;
    }
};

TEST_P(WatcherTest, DetectsCreate) {
    std::ofstream(proj / "src/new_file.ts") << "export const created = 1;\n";
    axon::WatchEvent acc;
    EXPECT_TRUE(wait_until(
        8s, [](const auto& e) { return !e.changed.empty(); }, acc))
        << "backend=" << backend;
    EXPECT_TRUE(has_changed(acc, "src/new_file.ts")) << "backend=" << backend;
}

TEST_P(WatcherTest, DetectsModify) {
    std::this_thread::sleep_for(50ms);
    std::ofstream(proj / "src/seed.ts", std::ios::app) << "export const more = 2;\n";
    axon::WatchEvent acc;
    EXPECT_TRUE(wait_until(
        8s, [](const auto& e) { return !e.changed.empty(); }, acc))
        << "backend=" << backend;
    EXPECT_TRUE(has_changed(acc, "src/seed.ts")) << "backend=" << backend;
}

TEST_P(WatcherTest, DetectsDelete) {
    fs::remove(proj / "src/seed.ts");
    axon::WatchEvent acc;
    EXPECT_TRUE(wait_until(
        8s, [](const auto& e) { return e.deleted; }, acc))
        << "backend=" << backend;
}

TEST_P(WatcherTest, DetectsFileInNewSubdirectory) {
    fs::create_directories(proj / "src/nested");
    std::ofstream(proj / "src/nested/deep.ts") << "export const deep = 3;\n";
    axon::WatchEvent acc;
    EXPECT_TRUE(wait_until(
        8s, [](const auto& e) { return !e.changed.empty(); }, acc))
        << "backend=" << backend;
    EXPECT_TRUE(has_changed(acc, "src/nested/deep.ts")) << "backend=" << backend;
}

TEST_P(WatcherTest, IgnoresSkipDirs) {
    fs::create_directories(proj / "node_modules");
    std::ofstream(proj / "node_modules/dep.ts") << "export const dep = 4;\n";
    // Give the backend time to (wrongly) surface it, then assert it didn't.
    axon::WatchEvent acc;
    wait_until(
        2s, [](const auto& e) { return !e.changed.empty(); }, acc);
    EXPECT_FALSE(has_changed(acc, "node_modules/dep.ts")) << "backend=" << backend;
}

INSTANTIATE_TEST_SUITE_P(Poll, WatcherTest, ::testing::Values(axon::WatchBackend::Poll));
INSTANTIATE_TEST_SUITE_P(Native, WatcherTest, ::testing::Values(axon::WatchBackend::Native));

// ── Factory behavior ─────────────────────────────────────────────────────────

TEST(WatcherFactory, ForcePollViaEnv) {
    auto proj = make_temp_project();
    testsupport::set_env("AXON_WATCH_FORCE_POLL", "1");
    axon::Config cfg;
    cfg.project_root = proj;
    std::string backend;
    auto w = axon::make_watcher(cfg, axon::WatchBackend::Auto, 200, backend);
    testsupport::unset_env("AXON_WATCH_FORCE_POLL");
    ASSERT_TRUE(w);
    EXPECT_STREQ(w->backend_name(), "poll");
    EXPECT_EQ(backend, "poll");
    w->stop();
    fs::remove_all(proj);
}

TEST(WatcherFactory, FallbackOnNativeInitFailure) {
    auto proj = make_temp_project();
    testsupport::set_env("AXON_WATCH_FORCE_NATIVE_FAIL", "1");
    axon::Config cfg;
    cfg.project_root = proj;
    std::string backend;
    auto w = axon::make_watcher(cfg, axon::WatchBackend::Auto, 200, backend);
    testsupport::unset_env("AXON_WATCH_FORCE_NATIVE_FAIL");
    ASSERT_TRUE(w) << "auto must fall back to poll when native init fails";
    EXPECT_EQ(backend, "poll");

    // The fallback watcher must actually work, not just exist.
    std::ofstream(proj / "src/after_fallback.ts") << "export const x = 1;\n";
    axon::WatchEvent ev;
    bool got = false;
    auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!got && std::chrono::steady_clock::now() < deadline) {
        axon::WatchEvent e;
        if (w->wait_for_changes(500ms, e)) {
            ev.merge(e);
            got = !ev.changed.empty();
        }
    }
    EXPECT_TRUE(got);
    w->stop();
    fs::remove_all(proj);
}

TEST(WatcherFactory, NativeModeFailsHardWhenForced) {
    auto proj = make_temp_project();
    testsupport::set_env("AXON_WATCH_FORCE_NATIVE_FAIL", "1");
    axon::Config cfg;
    cfg.project_root = proj;
    std::string backend;
    auto w = axon::make_watcher(cfg, axon::WatchBackend::Native, 200, backend);
    testsupport::unset_env("AXON_WATCH_FORCE_NATIVE_FAIL");
    EXPECT_EQ(w, nullptr) << "--backend=native must not silently fall back";
    fs::remove_all(proj);
}

#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
// Native detection must beat the polling interval — the reason this backend
// exists. Poll needs ~interval_ms; native should see the event well before.
TEST(WatcherLatency, NativeDetectsFasterThanPollInterval) {
    auto proj = make_temp_project();
    axon::Config cfg;
    cfg.project_root = proj;
    std::string backend;
    const int poll_interval_ms = 2000;
    auto w = axon::make_watcher(cfg, axon::WatchBackend::Native, poll_interval_ms, backend);
    if (!w) GTEST_SKIP() << "native watcher unavailable in this sandbox";

    std::this_thread::sleep_for(300ms); // let the stream settle (FSEvents)
    auto t0 = std::chrono::steady_clock::now();
    std::ofstream(proj / "src/latency_probe.ts") << "export const probe = 1;\n";
    axon::WatchEvent ev;
    bool got = w->wait_for_changes(std::chrono::milliseconds(poll_interval_ms), ev);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    EXPECT_TRUE(got) << "backend=" << backend;
    EXPECT_LT(elapsed.count(), poll_interval_ms)
        << "native detection should not need a full polling interval";
    w->stop();
    fs::remove_all(proj);
}
#endif

TEST(WatcherFactory, ForcedOverflowSeamFlagsFirstBatch) {
    auto proj = make_temp_project();
    testsupport::set_env("AXON_WATCH_FORCE_OVERFLOW", "1");
    axon::Config cfg;
    cfg.project_root = proj;
    std::string backend;
    auto w = axon::make_watcher(cfg, axon::WatchBackend::Poll, 200, backend);
    testsupport::unset_env("AXON_WATCH_FORCE_OVERFLOW");
    ASSERT_TRUE(w);

    std::ofstream(proj / "src/overflow_probe.ts") << "export const probe = 1;\n";
    axon::WatchEvent ev;
    bool got = false;
    auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!got && std::chrono::steady_clock::now() < deadline) {
        axon::WatchEvent e;
        if (w->wait_for_changes(500ms, e)) {
            ev.merge(e);
            got = true;
        }
    }
    ASSERT_TRUE(got);
    EXPECT_TRUE(ev.overflow) << "seam must flag the first batch as overflow";
    w->stop();
    fs::remove_all(proj);
}
