#include <gtest/gtest.h>
#include "core/registry.hpp"
#include "test_support.hpp"
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

// ── Fixture: every test runs against an isolated AXON_REGISTRY_DIR so nothing here
//    can touch the user's real ~/.axon/registry.json (the exact pollution
//    this override exists to prevent: e2e runs left 140 dead entries there). ─

class RegistryTest : public ::testing::Test {
protected:
    fs::path home;

    void SetUp() override {
        static int counter = 0;
        home = fs::temp_directory_path() /
               ("axon_registry_test_" + std::to_string(testsupport::pid()) + "_" +
                std::to_string(++counter));
        fs::create_directories(home);
        testsupport::set_env("AXON_REGISTRY_DIR", home.string());
    }

    void TearDown() override {
        testsupport::unset_env("AXON_REGISTRY_DIR");
        fs::remove_all(home);
    }
};

TEST_F(RegistryTest, AxonHomeOverridesRegistryLocation) {
    EXPECT_EQ(axon::registry_path(), home / "registry.json");
}

TEST_F(RegistryTest, RegisterRepoWritesUnderAxonHome) {
    fs::path repo = home / "repo";
    fs::create_directories(repo);
    axon::register_repo(repo.string(), (repo / ".axon/index.duckdb").string());
    EXPECT_TRUE(fs::exists(home / "registry.json"));

    auto reg = axon::load_registry();
    ASSERT_EQ(reg.repos.size(), 1u);
    EXPECT_EQ(reg.repos[0].root, repo.string());
}

TEST_F(RegistryTest, PruneRemovesDeadRootsKeepsLive) {
    fs::path live = home / "live-repo";
    fs::create_directories(live);
    axon::register_repo(live.string(), (live / ".axon/index.duckdb").string());
    axon::register_repo((home / "gone-repo").string(), "unused"); // dir never created

    EXPECT_EQ(axon::prune_registry(), 1);

    auto reg = axon::load_registry();
    ASSERT_EQ(reg.repos.size(), 1u);
    EXPECT_EQ(reg.repos[0].root, live.string());
}

TEST_F(RegistryTest, PruneKeepsDeadRootWithLiveOwner) {
    // A repo whose root vanished but whose registered owner process is still
    // alive must be kept — pruning it would clobber live owner bookkeeping.
    fs::path gone = home / "gone-but-owned";
    axon::register_repo(gone.string(), "unused");
    axon::set_repo_owner(gone.string(), (long long)testsupport::pid(), 4242, "tok");

    EXPECT_EQ(axon::prune_registry(), 0);
    auto reg = axon::load_registry();
    ASSERT_EQ(reg.repos.size(), 1u);
}

TEST_F(RegistryTest, PruneDropsGroupMembersOfPrunedRepos) {
    fs::path live = home / "live-repo";
    fs::create_directories(live);
    axon::register_repo(live.string(), "unused");
    axon::register_repo((home / "gone-repo").string(), "unused");

    auto reg = axon::load_registry();
    reg.groups.emplace_back("g1", std::vector<std::string>{"live-repo", "gone-repo"});
    axon::save_registry(reg);

    EXPECT_EQ(axon::prune_registry(), 1);

    auto after = axon::load_registry();
    ASSERT_EQ(after.groups.size(), 1u);
    EXPECT_EQ(after.groups[0].second, std::vector<std::string>{"live-repo"});
}

TEST_F(RegistryTest, PruneOnEmptyRegistryIsNoop) {
    EXPECT_EQ(axon::prune_registry(), 0);
}
