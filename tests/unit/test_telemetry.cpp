#include "core/db.hpp"
#include "core/telemetry.hpp"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

static fs::path make_temp_db() {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("axon_telemetry_test_" + std::to_string(stamp) + ".duckdb");
}

class TelemetryTest : public ::testing::Test {
protected:
    fs::path db_path;
    std::unique_ptr<axon::Database> db;
    axon::Config cfg;

    void SetUp() override {
        db_path = make_temp_db();
        db = std::make_unique<axon::Database>(db_path);
        cfg.project_cfg.telemetry = true;
    }

    void TearDown() override {
        db.reset();
        fs::remove(db_path);
    }
};

TEST_F(TelemetryTest, MetricsAreSeparatedByOptimizationLayer) {
    axon::record_telemetry(cfg, db.get(), {
        "get_context_capsule", "mcp", 10, 100, 400, 300, false, "retrieval"
    });
    axon::record_telemetry(cfg, db.get(), {
        "get_context_capsule", "mcp", 2, 20, 80, 60, true, ""
    });
    axon::record_telemetry(cfg, db.get(), {
        "compress_body", "internal", 1, 50, 200, 150, false, "compression"
    });
    axon::record_telemetry(cfg, db.get(), {
        "diff", "shell", 3, 25, 100, 75, false, "shell_filtering"
    });
    axon::record_telemetry(cfg, db.get(), {
        "artifact_retrieve", "internal", 1, 5, 5, 0, false, "ccr"
    });

    auto metrics = axon::metrics_json(cfg, db.get());

    EXPECT_EQ(metrics["requests"].get<int64_t>(), 5);
    EXPECT_EQ(metrics["tokens_sent"].get<int64_t>(), 200);
    EXPECT_EQ(metrics["tokens_saved"].get<int64_t>(), 585);

    const auto& layers = metrics["layers"];
    EXPECT_EQ(layers["retrieval"]["requests"].get<int64_t>(), 1);
    EXPECT_EQ(layers["retrieval"]["tokens_saved"].get<int64_t>(), 300);

    EXPECT_EQ(layers["cache"]["requests"].get<int64_t>(), 1);
    EXPECT_EQ(layers["cache"]["tokens_saved"].get<int64_t>(), 60);

    EXPECT_EQ(layers["compression"]["requests"].get<int64_t>(), 1);
    EXPECT_EQ(layers["compression"]["tokens_saved"].get<int64_t>(), 150);

    EXPECT_EQ(layers["shell_filtering"]["requests"].get<int64_t>(), 1);
    EXPECT_EQ(layers["shell_filtering"]["tokens_saved"].get<int64_t>(), 75);

    EXPECT_EQ(layers["ccr"]["requests"].get<int64_t>(), 1);
    EXPECT_EQ(layers["ccr"]["tokens_saved"].get<int64_t>(), 0);
}

TEST_F(TelemetryTest, LegacyEventsInferLayerSafely) {
    axon::record_telemetry(cfg, db.get(), {
        "capsule", "cli", 4, 80, 320, 240, false
    });
    axon::record_telemetry(cfg, db.get(), {
        "capsule", "cli", 1, 80, 320, 240, true
    });

    auto metrics = axon::metrics_json(cfg, db.get());
    EXPECT_EQ(metrics["layers"]["retrieval"]["requests"].get<int64_t>(), 1);
    EXPECT_EQ(metrics["layers"]["cache"]["requests"].get<int64_t>(), 1);
}
