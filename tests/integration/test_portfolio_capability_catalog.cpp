#include "portfolio/delivery/portfolio_capability_catalog.hpp"
#include "portfolio/application/candidates/capability_candidates.hpp"
#include "portfolio/application/declarations/capability_declarations.hpp"
#include "portfolio/infrastructure/git/git_process.hpp"
#include "core/config.hpp"
#include "core/db.hpp"
#include "core/indexer.hpp"
#include "portfolio/domain/index_journal.hpp"
#include <gtest/gtest.h>
#include <duckdb.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <algorithm>
#include <array>

namespace fs = std::filesystem;
namespace {
constexpr const char* kFirstRepository = "7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb";
constexpr const char* kFirstStream = "4b809f2e-5606-4f45-b050-e4dbb30cde53";
constexpr const char* kSecondRepository = "11c79d58-b6d0-4bdf-a2d4-3f3c1f0a8d91";
constexpr const char* kSecondStream = "618f802b-e97d-4f24-9cf7-62b09e4d1e62";
constexpr const char* kThirdRepository = "8b0539ee-b809-4f81-b2f6-df2f2b425a5a";
constexpr const char* kThirdStream = "214c6637-9a08-4227-a83e-0d73a2539e99";

std::string bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void git_ok(const fs::path& root, const std::vector<std::string>& arguments) {
    const auto result = axon::portfolio::git::run(root, arguments);
    ASSERT_EQ(result.exit_code, 0) << result.stdout_text;
}

struct SourceFile {
    std::string path, symbol, route, hash;
};

void create_source(const fs::path& source, const char* repository_id, const char* stream_id,
                   const std::vector<SourceFile>& files) {
    duckdb::DuckDB db(source.string());
    duckdb::Connection c(db);
    ASSERT_FALSE(
        c.Query("CREATE TABLE index_metadata(singleton BOOLEAN,repository_id "
                "VARCHAR,index_stream_id VARCHAR,current_epoch VARCHAR,current_manifest VARCHAR)")
            ->HasError());
    ASSERT_FALSE(c.Query("CREATE TABLE index_events(sequence UBIGINT)")->HasError());
    ASSERT_FALSE(c.Query("CREATE TABLE files(id BIGINT,path VARCHAR,language VARCHAR,hash VARCHAR)")
                     ->HasError());
    ASSERT_FALSE(c.Query("CREATE TABLE symbols(file_id BIGINT,name VARCHAR)")->HasError());
    ASSERT_FALSE(c.Query("CREATE TABLE routes(handler_file VARCHAR,method VARCHAR,path VARCHAR)")
                     ->HasError());
    ASSERT_FALSE(c.Query("CREATE TABLE edges(from_file BIGINT,to_file BIGINT)")->HasError());
    ASSERT_FALSE(
        c.Query(
             "CREATE TABLE external_dependencies(from_file BIGINT,specifier VARCHAR,kind VARCHAR)")
            ->HasError());
    ASSERT_FALSE(c.Query("CREATE TABLE capability_contexts(file_id BIGINT,bounded_context VARCHAR)")
                     ->HasError());
    ASSERT_FALSE(c.Query("CREATE TABLE capability_ast_fingerprints(file_id BIGINT,value VARCHAR)")
                     ->HasError());
    ASSERT_FALSE(c.Query("INSERT INTO index_metadata VALUES(true,'" + std::string(repository_id) +
                         "','" + stream_id + "','epoch-000000000001','manifest-000000000001')")
                     ->HasError());
    ASSERT_FALSE(c.Query("INSERT INTO index_events VALUES(1)")->HasError());
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto id = std::to_string(i + 1U);
        const auto& file = files[i];
        ASSERT_FALSE(c.Query("INSERT INTO files VALUES(" + id + ",'" + file.path + "','cpp','" +
                             file.hash + "')")
                         ->HasError());
        ASSERT_FALSE(
            c.Query("INSERT INTO symbols VALUES(" + id + ",'" + file.symbol + "')")->HasError());
        ASSERT_FALSE(
            c.Query("INSERT INTO routes VALUES('" + file.path + "','POST','" + file.route + "')")
                ->HasError());
        const auto slash = file.path.find('/', 4);
        if (file.path.rfind("src/", 0) == 0 && slash != std::string::npos) {
            const auto context = file.path.substr(4, slash - 4);
            ASSERT_FALSE(
                c.Query("INSERT INTO capability_contexts VALUES(" + id + ",'" + context + "')")
                    ->HasError());
        }
        ASSERT_FALSE(c.Query("INSERT INTO capability_ast_fingerprints VALUES(" + id + ",'" +
                             file.hash + "')")
                         ->HasError());
        if (file.path.find("payment_authorize") != std::string::npos) {
            ASSERT_FALSE(c.Query("INSERT INTO external_dependencies VALUES(" + id +
                                 ",'@axon/shared-contracts','imports')")
                             ->HasError());
        }
        if (file.path.find("search") != std::string::npos) {
            ASSERT_FALSE(c.Query("INSERT INTO edges VALUES(" + id + ",1)")->HasError());
        }
    }
}

TEST(PortfolioCapabilityCatalog, SyncsRealReadOnlySourcesAndSearchesMetadata) {
    const auto base = fs::temp_directory_path() / "axon-g13-catalog";
    fs::remove_all(base);
    fs::create_directories(base / "repo/.axon");
    fs::create_directories(base / "repo-two/.axon");
    fs::create_directories(base / "repo-three/.axon");
    const auto source = base / "repo/.axon/index.duckdb";
    const auto second_source = base / "repo-two/.axon/index.duckdb";
    const auto third_source = base / "repo-three/.axon/index.duckdb";
    create_source(
        source, kFirstRepository, kFirstStream,
        {{"src/billing/payment_authorize.cpp", "authorize", "/pay", "0123456789abcdef"},
         {"src/billing/invoice_create.cpp", "create_invoice", "/invoice", "1111111111111111"},
         {"src/billing/order_policy.cpp", "apply_policy", "/order", "2222222222222222"},
         {"src/billing/status_check.cpp", "status", "/status", "3333333333333333"},
         {"src/catalog/search.cpp", "query", "/search", "fedcba9876543210"}});
    create_source(
        second_source, kSecondRepository, kSecondStream,
        {{"src/billing/payment_authorize.cpp", "authorize", "/pay", "0123456789abcdef"},
         {"src/billing/invoice_create.cpp", "create_invoice", "/invoice", "9999999999999999"},
         {"src/billing/workflow_policy.cpp", "apply_policy", "/workflow", "2222222222222222"}});
    create_source(third_source, kThirdRepository, kThirdStream,
                  {{"src/billing/payment_authorize.cpp", "authorize", "/pay", "0123456789abcdef"},
                   {"src/identity/status_check.cpp", "status", "/status", "3333333333333333"}});
    std::ofstream(base / "repo/capabilities.json")
        << R"({"schema_version":"axon/capability-graph/v1","capabilities":[{"id":"payments.authorize","name":"payment authorize","bounded_context":"billing","contracts":["AuthorizeRequest"]},{"id":"platform.unimplemented","name":"unimplemented capability","bounded_context":"platform","contracts":[]}]})";
    git_ok(base / "repo", {"init", "--quiet"});
    git_ok(base / "repo", {"config", "user.email", "axon-tests@example.invalid"});
    git_ok(base / "repo", {"config", "user.name", "Axon Tests"});
    git_ok(base / "repo", {"add", "capabilities.json"});
    git_ok(base / "repo", {"commit", "--quiet", "-m", "capability fixture"});
    const auto before = bytes(source);
    const auto second_before = bytes(second_source);
    const auto third_before = bytes(third_source);
    fs::create_directories(base / "registry");
    std::ofstream(base / "registry/registry.json")
        << "{\"schema_version\":\"axon-registry/v2\",\"repos\":["
        << "{\"name\":\"repo\",\"root\":\"" << (base / "repo").string() << "\",\"db_path\":\""
        << source.string() << "\",\"repository_id\":\"" << kFirstRepository
        << "\",\"index_stream_id\":\"" << kFirstStream
        << "\",\"default_for_profiles\":[\"local\"]},"
        << "{\"name\":\"repo-two\",\"root\":\"" << (base / "repo-two").string()
        << "\",\"db_path\":\"" << second_source.string() << "\",\"repository_id\":\""
        << kSecondRepository << "\",\"index_stream_id\":\"" << kSecondStream
        << "\",\"default_for_profiles\":[\"local\"]},"
        << "{\"name\":\"repo-three\",\"root\":\"" << (base / "repo-three").string()
        << "\",\"db_path\":\"" << third_source.string() << "\",\"repository_id\":\""
        << kThirdRepository << "\",\"index_stream_id\":\"" << kThirdStream
        << "\",\"default_for_profiles\":[\"local\"]}],"
        << "\"storage_profiles\":{\"local\":{\"role\":\"portfolio_local\",\"transport\":\"local\","
           "\"default\":true,\"portfolio_store\":{\"provider\":\"duckdb\",\"path\":\"x\"}}}}";
    setenv("AXON_REGISTRY_DIR", (base / "registry").c_str(), 1);
    axon::portfolio::PortfolioCapabilityCatalog catalog(base / "catalog.duckdb");
    const auto report = catalog.sync();
    ASSERT_FALSE(report.degraded);
    ASSERT_EQ(report.repositories.size(), 3u);
    EXPECT_EQ(bytes(source), before);
    EXPECT_EQ(bytes(second_source), second_before);
    EXPECT_EQ(bytes(third_source), third_before);
    EXPECT_EQ(catalog.search("payment").size(), 3u);
    const auto signatures = catalog.list({}, 10);
    ASSERT_EQ(signatures.size(), 10u);
    const auto search =
        std::find_if(signatures.begin(), signatures.end(), [](const auto& signature) {
            return signature.path && *signature.path == "src/catalog/search.cpp";
        });
    ASSERT_NE(search, signatures.end());
    EXPECT_EQ(search->internal_dependencies,
              std::vector<std::string>({"src/billing/payment_authorize.cpp"}));
    EXPECT_EQ(std::count_if(signatures.begin(), signatures.end(),
                            [](const auto& signature) {
                                return signature.path &&
                                       *signature.path == "src/billing/payment_authorize.cpp" &&
                                       signature.external_dependencies ==
                                           std::vector<std::string>({"@axon/shared-contracts"});
                            }),
              3);
    EXPECT_FALSE(catalog.duplicates(0.0, 10).empty());

    // Every evidence signal below was read from the three source indexes; no candidate is enriched
    // after projection.
    const auto find = [&](const char* repository, const char* path) {
        const auto it =
            std::find_if(signatures.begin(), signatures.end(), [&](const auto& signature) {
                return signature.stream.repository_id == repository && signature.path &&
                       *signature.path == path;
            });
        EXPECT_NE(it, signatures.end());
        return *it;
    };
    auto payment_one = find(kFirstRepository, "src/billing/payment_authorize.cpp");
    auto payment_two = find(kSecondRepository, "src/billing/payment_authorize.cpp");
    EXPECT_EQ(axon::portfolio::CapabilityCandidateGenerator()
                  .generate({payment_one, payment_two})
                  .front()
                  .classification,
              axon::portfolio::CapabilityClassification::exact_duplicate);

    auto invoice_one = find(kFirstRepository, "src/billing/invoice_create.cpp");
    auto invoice_two = find(kSecondRepository, "src/billing/invoice_create.cpp");
    EXPECT_EQ(axon::portfolio::CapabilityCandidateGenerator()
                  .generate({invoice_one, invoice_two})
                  .front()
                  .classification,
              axon::portfolio::CapabilityClassification::convergent_capability);

    auto order = find(kFirstRepository, "src/billing/order_policy.cpp");
    auto workflow = find(kSecondRepository, "src/billing/workflow_policy.cpp");
    EXPECT_EQ(axon::portfolio::CapabilityCandidateGenerator()
                  .generate({order, workflow})
                  .front()
                  .classification,
              axon::portfolio::CapabilityClassification::local_specialization);

    auto status_one = find(kFirstRepository, "src/billing/status_check.cpp");
    auto status_three = find(kThirdRepository, "src/identity/status_check.cpp");
    EXPECT_EQ(axon::portfolio::CapabilityCandidateGenerator()
                  .generate({status_one, status_three})
                  .front()
                  .classification,
              axon::portfolio::CapabilityClassification::semantic_coincidence);

    const auto comparison = catalog.drift(base / "repo", "capabilities.json");
    EXPECT_FALSE(comparison.matches.empty());
    EXPECT_NE(
        std::find_if(comparison.drift.begin(), comparison.drift.end(),
                     [](const auto& drift) {
                         return drift.kind ==
                                axon::portfolio::CapabilityDriftKind::declaration_without_observed;
                     }),
        comparison.drift.end());
    EXPECT_NE(
        std::find_if(comparison.drift.begin(), comparison.drift.end(),
                     [](const auto& drift) {
                         return drift.kind ==
                                axon::portfolio::CapabilityDriftKind::observed_without_declaration;
                     }),
        comparison.drift.end());
}

TEST(PortfolioCapabilityCatalog, ProjectsIndexerProducedEvidenceFromThreeRepositories) {
    const auto base = fs::temp_directory_path() / "axon-g15-indexer-to-catalog";
    fs::remove_all(base);
    struct Repo {
        const char* id;
        const char* stream;
        fs::path root;
    };
    const std::array<Repo, 3> repos{{{kFirstRepository, kFirstStream, base / "one"},
                                     {kSecondRepository, kSecondStream, base / "two"},
                                     {kThirdRepository, kThirdStream, base / "three"}}};
    std::array<std::string, 3> streams;
    for (std::size_t i = 0; i < repos.size(); ++i) {
        const auto& repo = repos[i];
        fs::create_directories(repo.root / "src/billing");
        std::ofstream(repo.root / "repository-contract.yaml")
            << "schema_version: repository-contract/v1\nrepository_id: " << repo.id << "\n";
        std::ofstream(repo.root / "src/billing/payment_authorize.ts")
            << "export function authorize() { return true; }\n";
        auto cfg = axon::make_config(repo.root);
        axon::Database db(cfg.db_path);
        ASSERT_EQ(axon::index_project(cfg, db).files_indexed, 1);
        streams[i] = axon::portfolio::index_identity(db.conn()).index_stream_id;
        auto evidence = db.conn().Query("SELECT COUNT(*) FROM capability_ast_fingerprints");
        ASSERT_FALSE(evidence->HasError());
        ASSERT_EQ(evidence->GetValue<int64_t>(0, 0), 1);
    }
    fs::create_directories(base / "registry");
    std::ofstream registry(base / "registry/registry.json");
    registry << "{\"schema_version\":\"axon-registry/v2\",\"repos\":[";
    for (std::size_t i = 0; i < repos.size(); ++i) {
        if (i) registry << ',';
        const auto& repo = repos[i];
        registry << "{\"name\":\"r" << i << "\",\"root\":\"" << repo.root.string()
                 << "\",\"db_path\":\"" << (repo.root / ".axon/index.duckdb").string()
                 << "\",\"repository_id\":\"" << repo.id << "\",\"index_stream_id\":\""
                 << streams[i] << "\",\"default_for_profiles\":[\"local\"]}";
    }
    registry << "],\"storage_profiles\":{\"local\":{\"role\":\"portfolio_local\",\"transport\":"
                "\"local\",\"default\":true,\"portfolio_store\":{\"provider\":\"duckdb\",\"path\":"
                "\"x\"}}}}";
    registry.close();
    setenv("AXON_REGISTRY_DIR", (base / "registry").c_str(), 1);
    {
        auto cfg = axon::make_config(repos[0].root);
        axon::Database db(cfg.db_path);
        db.exec("DELETE FROM capability_contexts");
        db.exec("DELETE FROM capability_ast_fingerprints");
    }
    axon::portfolio::PortfolioCapabilityCatalog catalog(base / "catalog.duckdb");
    ASSERT_FALSE(catalog.sync().degraded);
    ASSERT_TRUE(catalog.list(kFirstRepository, 10).front().ast_fingerprints.empty());
    {
        auto cfg = axon::make_config(repos[0].root);
        axon::Database db(cfg.db_path);
        EXPECT_EQ(axon::index_project(cfg, db).files_indexed, 0);
        EXPECT_EQ(db.conn()
                      .Query("SELECT COUNT(*) FROM capability_ast_fingerprints")
                      ->GetValue<int64_t>(0, 0),
                  1);
    }
    const auto refreshed = catalog.sync();
    ASSERT_FALSE(refreshed.degraded);
    const auto refreshed_first =
        std::find_if(refreshed.repositories.begin(), refreshed.repositories.end(),
                     [](const auto& item) { return item.repository_id == kFirstRepository; });
    ASSERT_NE(refreshed_first, refreshed.repositories.end());
    EXPECT_EQ(refreshed_first->status, "synced");
    ASSERT_FALSE(catalog.list(kFirstRepository, 10).front().ast_fingerprints.empty());
    const auto candidates = catalog.duplicates(0.0, 10);
    ASSERT_FALSE(candidates.empty());
    EXPECT_EQ(candidates.front().classification,
              axon::portfolio::CapabilityClassification::exact_duplicate);
}
} // namespace
