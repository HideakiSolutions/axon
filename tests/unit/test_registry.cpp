#include <gtest/gtest.h>
#include "core/registry.hpp"
#include "test_support.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <unordered_set>

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
    EXPECT_GT(reg.repos[0].owner_started_at, 0);
    EXPECT_GT(reg.repos[0].owner_heartbeat_at, 0);
}

TEST_F(RegistryTest, HeartbeatOnlyUpdatesCurrentOwner) {
    fs::path repo = home / "live-repo";
    fs::create_directories(repo);
    axon::register_repo(repo.string(), "unused");
    axon::set_repo_owner(repo.string(), (long long)testsupport::pid(), 4242, "tok");

    EXPECT_FALSE(axon::touch_repo_owner(repo.string(), 2147483647LL));
    EXPECT_TRUE(axon::touch_repo_owner(repo.string(), (long long)testsupport::pid()));
    auto entry = axon::find_repo(repo.string());
    ASSERT_TRUE(entry.has_value());
    EXPECT_GT(entry->owner_heartbeat_at, 0);
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

TEST_F(RegistryTest, PruneClearsDeadOwnerOnLiveRoot) {
    // A crashed/SIGKILLed serve never runs clear_repo_owner, leaving stale
    // owner bookkeeping on a perfectly live repo. Prune must keep the entry
    // but zero the owner fields. 2147483647 exceeds Linux's default pid_max
    // and is not a valid (multiple-of-4) Windows pid.
    fs::path live = home / "live-repo";
    fs::create_directories(live);
    axon::register_repo(live.string(), "unused");
    axon::set_repo_owner(live.string(), 2147483647LL, 4242, "tok");

    EXPECT_EQ(axon::prune_registry(), 0) << "live root must not be removed";

    auto reg = axon::load_registry();
    ASSERT_EQ(reg.repos.size(), 1u);
    EXPECT_EQ(reg.repos[0].owner_pid, 0);
    EXPECT_EQ(reg.repos[0].owner_port, 0);
    EXPECT_TRUE(reg.repos[0].owner_token.empty());
    EXPECT_EQ(reg.repos[0].owner_started_at, 0);
    EXPECT_EQ(reg.repos[0].owner_heartbeat_at, 0);
}

TEST_F(RegistryTest, CountPrunableMatchesPruneSemantics) {
    fs::path live = home / "live-repo";
    fs::create_directories(live);
    axon::register_repo(live.string(), "unused");          // live root → kept
    axon::register_repo((home / "gone-a").string(), "un"); // dead → prunable
    axon::register_repo((home / "gone-b").string(), "un"); // dead but owned by a
                                                           // live process → kept
    axon::set_repo_owner((home / "gone-b").string(), (long long)testsupport::pid(), 1, "t");

    EXPECT_EQ(axon::count_prunable(axon::load_registry()), 1);
    EXPECT_EQ(axon::prune_registry(), 1);
    EXPECT_EQ(axon::count_prunable(axon::load_registry()), 0);
}

TEST_F(RegistryTest, V1RegistryRoundTripsWithoutV2Fields) {
    fs::path repo = home / "legacy";
    fs::create_directories(repo);
    axon::register_repo(repo.string(), "legacy.duckdb");
    auto reg = axon::load_registry();
    EXPECT_TRUE(reg.schema_version.empty());
    ASSERT_TRUE(axon::validate_registry(reg).empty());
    axon::save_registry(reg);

    std::ifstream input(home / "registry.json");
    nlohmann::json saved;
    input >> saved;
    EXPECT_FALSE(saved.contains("schema_version"));
    EXPECT_FALSE(saved.contains("storage_profiles"));
    EXPECT_FALSE(saved["repos"][0].contains("repository_id"));
}

TEST_F(RegistryTest, V2ProfilesAndWorktreeStreamsRoundTrip) {
    axon::RegistryData reg;
    reg.schema_version = "axon-registry/v2";
    reg.storage_profiles = {
        axon::StorageProfile{"local", "portfolio_local", "", "", "",
                             axon::ProviderTarget{"duckdb", "portfolio.duckdb", ""},
                             std::nullopt, std::nullopt, std::nullopt, true},
        axon::StorageProfile{"shared-dev", "portfolio_shared", "axon_http",
                             "http://127.0.0.1:7071", "axon",
                             axon::ProviderTarget{"postgresql", "", ""},
                             axon::ProviderTarget{"qdrant", "", ""},
                             axon::ProviderTarget{"falkordb", "", ""},
                             axon::TargetMarker{"shared-1", "axon",
                                                "axon/portfolio-sync/v1"},
                             false}};
    axon::RepoEntry main;
    main.name = "repo-main";
    main.root = (home / "main").string();
    main.db_path = (home / "main/.axon/index.duckdb").string();
    main.repository_id = "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb";
    main.index_stream_id = "11111111-1111-4111-8111-111111111111";
    main.variant = "main";
    main.default_for_profiles = {"local", "shared-dev"};
    axon::RepoEntry worktree = main;
    worktree.name = "repo-worktree";
    worktree.root = (home / "worktree").string();
    worktree.db_path = (home / "worktree/.axon/index.duckdb").string();
    worktree.index_stream_id = "22222222-2222-4222-8222-222222222222";
    worktree.variant = "worktree";
    worktree.default_for_profiles.clear();
    reg.repos = {main, worktree};
    reg.groups = {{"core", {main.repository_id, main.name}}};

    EXPECT_TRUE(axon::validate_registry(reg).empty());
    axon::save_registry(reg);
    auto loaded = axon::load_registry();
    ASSERT_EQ(loaded.repos.size(), 2u);
    ASSERT_EQ(loaded.storage_profiles.size(), 2u);
    EXPECT_EQ(loaded.schema_version, "axon-registry/v2");
    EXPECT_EQ(loaded.repos[0].repository_id, main.repository_id);
    EXPECT_EQ(loaded.repos[1].index_stream_id, worktree.index_stream_id);
    ASSERT_TRUE(axon::default_storage_profile(loaded).has_value());
    EXPECT_EQ(axon::default_storage_profile(loaded)->name, "local");
    ASSERT_TRUE(axon::default_repo_stream(loaded, main.repository_id, "shared-dev").has_value());
    EXPECT_EQ(axon::default_repo_stream(loaded, main.repository_id, "shared-dev")->index_stream_id,
              main.index_stream_id);
    auto selected = axon::aggregation_repos(loaded, "core");
    ASSERT_TRUE(selected.issues.empty());
    ASSERT_EQ(selected.repos.size(), 1u);
    EXPECT_EQ(selected.repos[0].variant, "main");
    EXPECT_EQ(axon::get_group_repos(loaded, "core").size(), 2u)
        << "mixed UUID/name membership must deduplicate each physical stream";

    std::ifstream input(home / "registry.json");
    nlohmann::json saved;
    input >> saved;
    EXPECT_EQ(saved["schema_version"], "axon-registry/v2");
    EXPECT_EQ(saved["groups"]["core"][0], main.repository_id);
    EXPECT_EQ(saved["storage_profiles"]["local"]["role"], "portfolio_local");
    EXPECT_EQ(saved["storage_profiles"]["shared-dev"]["providers"]["semantic_index"],
              "qdrant");
}

TEST_F(RegistryTest, V2RejectsDuplicateStreamAndAmbiguousDefaults) {
    axon::RegistryData reg;
    reg.schema_version = "axon-registry/v2";
    reg.storage_profiles = {
        axon::StorageProfile{"local", "portfolio_local", "", "", "",
                             axon::ProviderTarget{"duckdb", "portfolio.duckdb", ""},
                             std::nullopt, std::nullopt, std::nullopt, true}};
    axon::RepoEntry first;
    first.root = "/repo/a";
    first.repository_id = "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb";
    first.index_stream_id = "11111111-1111-4111-8111-111111111111";
    first.default_for_profiles = {"local"};
    axon::RepoEntry second = first;
    second.root = "/repo/b";
    reg.repos = {first, second};

    auto issues = axon::validate_registry(reg);
    std::unordered_set<std::string> codes;
    for (const auto& issue : issues) codes.insert(issue.code);
    EXPECT_TRUE(codes.count("duplicate_stream_binding"));
    EXPECT_TRUE(codes.count("ambiguous_default_stream"));
    EXPECT_TRUE(axon::aggregation_repos(reg).repos.empty());
    EXPECT_FALSE(axon::aggregation_repos(reg).issues.empty());
}

TEST_F(RegistryTest, V2RejectsTargetMarkerMismatch) {
    axon::RegistryData reg;
    reg.schema_version = "axon-registry/v2";
    reg.storage_profiles = {
        axon::StorageProfile{"shared", "portfolio_shared", "axon_http",
                             "http://127.0.0.1:7071", "axon",
                             axon::ProviderTarget{"postgresql", "", ""}, std::nullopt,
                             std::nullopt, axon::TargetMarker{"instance", "other", "v1"}, true}};
    auto issues = axon::validate_registry(reg);
    ASSERT_FALSE(issues.empty());
    EXPECT_TRUE(std::any_of(issues.begin(), issues.end(), [](const auto& issue) {
        return issue.code == "target_marker_mismatch";
    }));
    EXPECT_FALSE(axon::aggregation_repos(reg).issues.empty());
}

TEST_F(RegistryTest, V2RejectsUnsupportedVersionAndCombinedTarget) {
    axon::RegistryData reg;
    reg.schema_version = "axon-registry/v3";
    reg.storage_profiles = {
        axon::StorageProfile{"local", "portfolio_local", "", "https://wrong.example", "",
                             axon::ProviderTarget{"duckdb", "portfolio.duckdb", ""},
                             std::nullopt, std::nullopt, std::nullopt, true}};
    auto issues = axon::validate_registry(reg);
    std::unordered_set<std::string> codes;
    for (const auto& issue : issues) codes.insert(issue.code);
    EXPECT_TRUE(codes.count("unsupported_schema_version"));
    EXPECT_TRUE(codes.count("invalid_local_target"));
    EXPECT_FALSE(axon::aggregation_repos(reg).issues.empty());
}

TEST_F(RegistryTest, V2RejectsNonTlsNonLoopbackEndpoint) {
    axon::RegistryData reg;
    reg.schema_version = "axon-registry/v2";
    reg.storage_profiles = {
        axon::StorageProfile{"shared", "portfolio_shared", "axon_http",
                             "http://10.0.0.5:7071", "axon",
                             axon::ProviderTarget{"postgresql", "", ""}, std::nullopt,
                             std::nullopt,
                             axon::TargetMarker{"instance", "axon",
                                                "axon/portfolio-sync/v1"},
                             true}};
    auto issues = axon::validate_registry(reg);
    EXPECT_TRUE(std::any_of(issues.begin(), issues.end(), [](const auto& issue) {
        return issue.code == "invalid_shared_endpoint";
    }));
}

TEST_F(RegistryTest, MalformedFieldTypesFailClosedWithoutOverwrite) {
    const std::string malformed =
        R"({"schema_version":2,"repos":[],"groups":{},"storage_profiles":{}})";
    {
        std::ofstream output(home / "registry.json");
        output << malformed;
    }
    auto loaded = axon::load_registry();
    ASSERT_EQ(loaded.load_issues.size(), 1u);
    EXPECT_EQ(loaded.load_issues[0].code, "invalid_registry_type");
    auto selected = axon::aggregation_repos(loaded);
    EXPECT_TRUE(selected.repos.empty());
    ASSERT_FALSE(selected.issues.empty());
    EXPECT_EQ(selected.issues[0].code, "invalid_registry_type");

    axon::register_repo((home / "must-not-appear").string(), "index.duckdb");
    std::ifstream input(home / "registry.json");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()),
              malformed);
}

TEST_F(RegistryTest, MalformedStructuralTypesFailClosedWithoutOverwrite) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {R"([])", "$"},
        {R"({"repos":{}})", "$.repos"},
        {R"({"repos":[1]})", "$.repos[0]"},
        {R"({"repos":[{"name":1}]})", "$.repos[0].name"},
        {R"({"repos":[{"default_for_profiles":{}}]})",
         "$.repos[0].default_for_profiles"},
        {R"({"repos":[{"default_for_profiles":[1]}]})",
         "$.repos[0].default_for_profiles[0]"},
        {R"({"groups":[]})", "$.groups"},
        {R"({"groups":{"core":{}}})", "$.groups.core"},
        {R"({"groups":{"core":[1]}})", "$.groups.core[0]"},
        {R"({"storage_profiles":[]})", "$.storage_profiles"},
        {R"({"storage_profiles":{"local":[]}})", "$.storage_profiles.local"},
        {R"({"storage_profiles":{"local":{"role":1}}})",
         "$.storage_profiles.local.role"},
        {R"({"storage_profiles":{"local":{"default":"yes"}}})",
         "$.storage_profiles.local.default"},
        {R"({"storage_profiles":{"local":{"portfolio_store":[]}}})",
         "$.storage_profiles.local.portfolio_store"},
        {R"({"storage_profiles":{"local":{"semantic_index":"qdrant"}}})",
         "$.storage_profiles.local.semantic_index"},
        {R"({"storage_profiles":{"local":{"graph_projection":[]}}})",
         "$.storage_profiles.local.graph_projection"},
        {R"({"storage_profiles":{"shared":{"providers":[]}}})",
         "$.storage_profiles.shared.providers"},
        {R"({"storage_profiles":{"shared":{"providers":{"portfolio_store":{}}}}})",
         "$.storage_profiles.shared.providers.portfolio_store"},
        {R"({"storage_profiles":{"shared":{"target_marker":[]}}})",
         "$.storage_profiles.shared.target_marker"},
        {R"({"storage_profiles":{"shared":{"target_marker":{"protocol_version":1}}}})",
         "$.storage_profiles.shared.target_marker.protocol_version"}};

    for (size_t i = 0; i < cases.size(); ++i) {
        SCOPED_TRACE("case " + std::to_string(i) + ": " + cases[i].first);
        {
            std::ofstream output(home / "registry.json", std::ios::trunc);
            output << cases[i].first;
        }
        auto loaded = axon::load_registry();
        ASSERT_FALSE(loaded.load_issues.empty());
        EXPECT_TRUE(std::any_of(loaded.load_issues.begin(), loaded.load_issues.end(),
                                [&](const auto& issue) {
                                    return issue.code == "invalid_registry_type" &&
                                           issue.path == cases[i].second;
                                }));
        auto selected = axon::aggregation_repos(loaded);
        EXPECT_TRUE(selected.repos.empty());
        EXPECT_FALSE(selected.issues.empty());

        axon::register_repo((home / "must-not-appear").string(), "index.duckdb");
        std::ifstream input(home / "registry.json");
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>()),
                  cases[i].first);
    }
}

TEST_F(RegistryTest, LoadsAcceptedCanonicalV2Json) {
    nlohmann::json document = {
        {"schema_version", "axon-registry/v2"},
        {"repos",
         {{{"repository_id", "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb"},
           {"index_stream_id", "11111111-1111-4111-8111-111111111111"},
           {"name", "axon"},
           {"root", "/opt/hideakisolutions/axon"},
           {"db_path", "/opt/hideakisolutions/axon/.axon/index.duckdb"},
           {"variant", "main"},
           {"default_for_profiles", {"local", "team"}}}}},
        {"groups", {{"core", {"7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb"}}}},
        {"storage_profiles",
         {{"local",
           {{"role", "portfolio_local"},
            {"provider", "duckdb"},
            {"path", "${AXON_REGISTRY_DIR}/portfolio.duckdb"},
            {"default", true}}},
          {"team",
           {{"role", "portfolio_shared"},
            {"transport", "axon_http"},
            {"endpoint", "https://axon.internal.example"},
            {"namespace", "hideaki-portfolio"},
            {"providers",
             {{"portfolio_store", "postgresql"},
              {"semantic_index", "qdrant"},
              {"graph_projection", "falkordb"}}},
            {"target_marker",
             {{"instance_id", "shared-1"},
              {"namespace", "hideaki-portfolio"},
              {"protocol_version", "axon/portfolio-sync/v1"}}},
            {"default", false}}}}}};
    {
        std::ofstream output(home / "registry.json");
        output << document.dump(2) << '\n';
    }
    auto loaded = axon::load_registry();
    ASSERT_TRUE(axon::validate_registry(loaded).empty());
    ASSERT_EQ(loaded.storage_profiles.size(), 2u);
    EXPECT_EQ(loaded.storage_profiles[0].role, "portfolio_local");
    EXPECT_EQ(loaded.storage_profiles[1].portfolio_store.provider, "postgresql");
    EXPECT_EQ(axon::get_group_repos(loaded, "core").size(), 1u);
}

TEST_F(RegistryTest, SecondaryIsReadOnlyAndByteIdenticalAfterQuery) {
    fs::path repo = home / "secondary";
    fs::create_directories(repo / ".axon");
    fs::path db_path = repo / ".axon/index.duckdb";
    {
        duckdb::DuckDB db(db_path.string());
        duckdb::Connection conn(db);
        ASSERT_FALSE(conn.Query("CREATE TABLE facts(id INTEGER)")->HasError());
        ASSERT_FALSE(conn.Query("INSERT INTO facts VALUES (1)")->HasError());
    }
    auto bytes = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const auto before = bytes(db_path);
    axon::RepoEntry entry;
    entry.root = repo.string();
    entry.db_path = db_path.string();
    auto secondary = axon::open_secondary_read_only(entry);
    ASSERT_TRUE(secondary) << secondary.error;
    {
        duckdb::Connection conn(*secondary.db);
        auto read = conn.Query("SELECT * FROM facts");
        ASSERT_FALSE(read->HasError());
        ASSERT_EQ(read->RowCount(), 1u);
        EXPECT_TRUE(conn.Query("INSERT INTO facts VALUES (2)")->HasError());
    }
    secondary.db.reset();
    EXPECT_EQ(bytes(db_path), before);
}

TEST_F(RegistryTest, SecondaryRejectsSymlinkAndPathOutsideRoot) {
    fs::path repo = home / "repo";
    fs::path outside = home / "outside";
    fs::create_directories(repo / ".axon");
    fs::create_directories(outside);
    fs::path external_db = outside / "index.duckdb";
    {
        duckdb::DuckDB db(external_db.string());
    }
    axon::RepoEntry outside_entry;
    outside_entry.root = repo.string();
    outside_entry.db_path = external_db.string();
    auto rejected = axon::open_secondary_read_only(outside_entry);
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.error_code, "path_outside_root");

#ifndef _WIN32
    fs::path link = repo / ".axon/index.duckdb";
    fs::create_symlink(external_db, link);
    outside_entry.db_path = link.string();
    rejected = axon::open_secondary_read_only(outside_entry);
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.error_code, "symlink_rejected");
#endif
}
