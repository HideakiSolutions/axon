#include "portfolio/delivery/portfolio_capability_catalog.hpp"
#include <gtest/gtest.h>
#include <duckdb.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <algorithm>

namespace fs=std::filesystem;
namespace {
constexpr const char* kFirstRepository="7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb";
constexpr const char* kFirstStream="4b809f2e-5606-4f45-b050-e4dbb30cde53";
constexpr const char* kSecondRepository="11c79d58-b6d0-4bdf-a2d4-3f3c1f0a8d91";
constexpr const char* kSecondStream="618f802b-e97d-4f24-9cf7-62b09e4d1e62";

std::string bytes(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(in),{}};
}

void create_source(const fs::path& source, const char* repository_id, const char* stream_id,
                   const std::string& payment_path, const bool include_search) {
    duckdb::DuckDB db(source.string());
    duckdb::Connection c(db);
    ASSERT_FALSE(c.Query("CREATE TABLE index_metadata(singleton BOOLEAN,repository_id VARCHAR,index_stream_id VARCHAR,current_epoch VARCHAR,current_manifest VARCHAR)")->HasError());
    ASSERT_FALSE(c.Query("CREATE TABLE index_events(sequence UBIGINT)")->HasError());
    ASSERT_FALSE(c.Query("CREATE TABLE files(id BIGINT,path VARCHAR,language VARCHAR,hash VARCHAR)")->HasError());
    ASSERT_FALSE(c.Query("CREATE TABLE symbols(file_id BIGINT,name VARCHAR)")->HasError());
    ASSERT_FALSE(c.Query("CREATE TABLE routes(handler_file VARCHAR,method VARCHAR,path VARCHAR)")->HasError());
    ASSERT_FALSE(c.Query("CREATE TABLE edges(from_file BIGINT,to_file BIGINT)")->HasError());
    ASSERT_FALSE(c.Query("CREATE TABLE external_dependencies(from_file BIGINT,specifier VARCHAR,kind VARCHAR)")->HasError());
    ASSERT_FALSE(c.Query("INSERT INTO index_metadata VALUES(true,'"+std::string(repository_id)+"','"+stream_id+"','epoch-000000000001','manifest-000000000001')")->HasError());
    ASSERT_FALSE(c.Query("INSERT INTO index_events VALUES(1)")->HasError());
    ASSERT_FALSE(c.Query("INSERT INTO files VALUES(1,'"+payment_path+"','cpp','0123456789abcdef')")->HasError());
    ASSERT_FALSE(c.Query("INSERT INTO symbols VALUES(1,'authorize')")->HasError());
    ASSERT_FALSE(c.Query("INSERT INTO routes VALUES('"+payment_path+"','POST','/pay')")->HasError());
    ASSERT_FALSE(c.Query("INSERT INTO external_dependencies VALUES(1,'@axon/shared-contracts','imports')")->HasError());
    if (include_search) {
        ASSERT_FALSE(c.Query("INSERT INTO files VALUES(2,'src/search.cpp','cpp','fedcba9876543210')")->HasError());
        ASSERT_FALSE(c.Query("INSERT INTO symbols VALUES(2,'query')")->HasError());
        ASSERT_FALSE(c.Query("INSERT INTO edges VALUES(2,1)")->HasError());
    }
}

TEST(PortfolioCapabilityCatalog, SyncsRealReadOnlySourcesAndSearchesMetadata) {
    const auto base=fs::temp_directory_path()/"axon-g13-catalog";
    fs::remove_all(base);
    fs::create_directories(base/"repo/.axon");
    fs::create_directories(base/"repo-two/.axon");
    const auto source=base/"repo/.axon/index.duckdb";
    const auto second_source=base/"repo-two/.axon/index.duckdb";
    create_source(source,kFirstRepository,kFirstStream,"src/payment.cpp",true);
    create_source(second_source,kSecondRepository,kSecondStream,"src/payment.cpp",false);
    const auto before=bytes(source);
    const auto second_before=bytes(second_source);
    fs::create_directories(base/"registry");
    std::ofstream(base/"registry/registry.json")
        <<"{\"schema_version\":\"axon-registry/v2\",\"repos\":["
        <<"{\"name\":\"repo\",\"root\":\""<<(base/"repo").string()<<"\",\"db_path\":\""<<source.string()<<"\",\"repository_id\":\""<<kFirstRepository<<"\",\"index_stream_id\":\""<<kFirstStream<<"\",\"default_for_profiles\":[\"local\"]},"
        <<"{\"name\":\"repo-two\",\"root\":\""<<(base/"repo-two").string()<<"\",\"db_path\":\""<<second_source.string()<<"\",\"repository_id\":\""<<kSecondRepository<<"\",\"index_stream_id\":\""<<kSecondStream<<"\",\"default_for_profiles\":[\"local\"]}],"
        <<"\"storage_profiles\":{\"local\":{\"role\":\"portfolio_local\",\"transport\":\"local\",\"default\":true,\"portfolio_store\":{\"provider\":\"duckdb\",\"path\":\"x\"}}}}";
    setenv("AXON_REGISTRY_DIR",(base/"registry").c_str(),1);
    axon::portfolio::PortfolioCapabilityCatalog catalog(base/"catalog.duckdb");
    const auto report=catalog.sync();
    ASSERT_FALSE(report.degraded);
    ASSERT_EQ(report.repositories.size(),2u);
    EXPECT_EQ(bytes(source),before);
    EXPECT_EQ(bytes(second_source),second_before);
    EXPECT_EQ(catalog.search("payment").size(),2u);
    const auto signatures=catalog.list({},10);
    ASSERT_EQ(signatures.size(),3u);
    const auto search=std::find_if(signatures.begin(),signatures.end(),[](const auto& signature) {
        return signature.path && *signature.path=="src/search.cpp";
    });
    ASSERT_NE(search,signatures.end());
    EXPECT_EQ(search->internal_dependencies,std::vector<std::string>({"src/payment.cpp"}));
    EXPECT_EQ(std::count_if(signatures.begin(),signatures.end(),[](const auto& signature) {
        return signature.path && *signature.path=="src/payment.cpp" &&
            signature.external_dependencies==std::vector<std::string>({"@axon/shared-contracts"});
    }),2);
    EXPECT_FALSE(catalog.duplicates(0.0,10).empty());
}
}
