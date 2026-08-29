#include <gtest/gtest.h>

#include "core/pending_writes.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class PendingWritesTest : public ::testing::Test {
protected:
    fs::path axon_dir;

    void SetUp() override {
        static int counter = 0;
        axon_dir = fs::temp_directory_path() /
                   ("axon_pending_writes_test_" + std::to_string(testsupport::pid()) + "_" +
                    std::to_string(++counter));
        fs::create_directories(axon_dir);
    }

    void TearDown() override { fs::remove_all(axon_dir); }

    void write_queue(const std::string& value) {
        std::ofstream output(axon_dir / "pending-writes.txt", std::ios::trunc);
        output << value;
    }
};

TEST_F(PendingWritesTest, ClaimRemainsUntilAcknowledgedAndIsReplayed) {
    write_queue("src/a.cpp\nsrc/a.cpp\nsrc/b.cpp\n");

    {
        auto claim = axon::PendingWriteClaim::acquire(axon_dir);
        ASSERT_TRUE(claim.has_batch());
        EXPECT_EQ(claim.attempt(), 1);
        ASSERT_EQ(claim.paths().size(), 2u);
        EXPECT_TRUE(fs::exists(axon_dir / "pending-writes.processing"));
        EXPECT_FALSE(fs::exists(axon_dir / "pending-writes.txt"));
    }

    auto replay = axon::PendingWriteClaim::acquire(axon_dir);
    ASSERT_TRUE(replay.has_batch());
    EXPECT_EQ(replay.attempt(), 2);
    ASSERT_EQ(replay.paths().size(), 2u);
    replay.acknowledge();

    EXPECT_FALSE(fs::exists(axon_dir / "pending-writes.processing"));
    EXPECT_FALSE(fs::exists(axon_dir / "pending-writes.processing.attempts"));
}

TEST_F(PendingWritesTest, PoisonBatchIsQuarantinedAfterBoundedRetries) {
    write_queue("src/poison.cpp\n");

    auto first = axon::PendingWriteClaim::acquire(axon_dir, 2);
    ASSERT_TRUE(first.has_batch());
    EXPECT_EQ(first.attempt(), 1);

    auto second = axon::PendingWriteClaim::acquire(axon_dir, 2);
    ASSERT_TRUE(second.has_batch());
    EXPECT_EQ(second.attempt(), 2);

    auto quarantined = axon::PendingWriteClaim::acquire(axon_dir, 2);
    EXPECT_FALSE(quarantined.has_batch());
    ASSERT_TRUE(quarantined.quarantined_path().has_value());
    EXPECT_TRUE(fs::exists(*quarantined.quarantined_path()));
    EXPECT_FALSE(fs::exists(axon_dir / "pending-writes.processing"));
}

TEST_F(PendingWritesTest, QuarantineDoesNotBlockTheNextQueue) {
    write_queue("src/old.cpp\n");
    auto first = axon::PendingWriteClaim::acquire(axon_dir, 1);
    ASSERT_TRUE(first.has_batch());

    write_queue("src/new.cpp\n");
    auto next = axon::PendingWriteClaim::acquire(axon_dir, 1);
    ASSERT_TRUE(next.quarantined_path().has_value());
    ASSERT_TRUE(next.has_batch());
    ASSERT_EQ(next.paths().size(), 1u);
    EXPECT_EQ(next.paths()[0], fs::path("src/new.cpp"));
    next.acknowledge();
}

TEST_F(PendingWritesTest, EmptyQueueProducesNoClaim) {
    write_queue("");
    auto claim = axon::PendingWriteClaim::acquire(axon_dir);
    EXPECT_FALSE(claim.has_batch());
    EXPECT_FALSE(fs::exists(axon_dir / "pending-writes.processing"));
}
