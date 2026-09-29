#include "core/ccr.hpp"
#include "core/db.hpp"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

static fs::path make_temp_db() {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("axon_ccr_test_" + std::to_string(stamp) + ".duckdb");
}

class CcrTest : public ::testing::Test {
protected:
    fs::path db_path;
    std::unique_ptr<axon::Database> db;

    void SetUp() override {
        db_path = make_temp_db();
        db = std::make_unique<axon::Database>(db_path);
    }

    void TearDown() override {
        db.reset();
        fs::remove(db_path);
    }
};

TEST_F(CcrTest, ArtifactIdIsDeterministicAndContentSensitive) {
    std::string a = axon::ccr_artifact_id("capsule_body", "src/a.cpp:1-2", "return 1;\n");
    std::string b = axon::ccr_artifact_id("capsule_body", "src/a.cpp:1-2", "return 1;\n");
    std::string c = axon::ccr_artifact_id("capsule_body", "src/a.cpp:1-2", "return 2;\n");

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_EQ(a.rfind("ccr_", 0), 0u);
}

TEST_F(CcrTest, StoreAndRetrieveRoundTripsOriginalContent) {
    std::string content = "int f() {\n    int x = 1;\n    return x;\n}\n";
    std::string id = axon::ccr_store_artifact(*db, "capsule_body", "src/a.cpp:1-4", content, 12);

    auto artifact = axon::ccr_retrieve_artifact(*db, id);
    ASSERT_TRUE(artifact.has_value());
    EXPECT_EQ(artifact->artifact_id, id);
    EXPECT_EQ(artifact->kind, "capsule_body");
    EXPECT_EQ(artifact->source_ref, "src/a.cpp:1-4");
    EXPECT_EQ(artifact->content, content);
    EXPECT_EQ(artifact->token_estimate, 12);
}

TEST_F(CcrTest, StoreIsIdempotentForSameArtifact) {
    std::string content = "same content\n";
    std::string id1 = axon::ccr_store_artifact(*db, "capsule_body", "src/a:1-1", content, 3);
    std::string id2 = axon::ccr_store_artifact(*db, "capsule_body", "src/a:1-1", content, 3);

    EXPECT_EQ(id1, id2);
    auto count = db->conn().Query("SELECT COUNT(*) FROM ccr_artifacts");
    ASSERT_FALSE(count->HasError());
    EXPECT_EQ(count->GetValue<int64_t>(0, 0), 1);
}

TEST_F(CcrTest, MissingArtifactReturnsNullopt) {
    EXPECT_FALSE(axon::ccr_retrieve_artifact(*db, "ccr_missing").has_value());
}

TEST_F(CcrTest, MarkerIncludesArtifactIdAndOriginalTokens) {
    std::string marker = axon::ccr_marker("ccr_abc", 42);
    EXPECT_NE(marker.find("artifact_id=ccr_abc"), std::string::npos);
    EXPECT_NE(marker.find("original_tokens=42"), std::string::npos);
}

TEST_F(CcrTest, RecoverableOutputStoresOriginalAndPrependsMarker) {
    std::string original(4000, 'x');
    std::string lossy = "# summary\nimportant line\n";

    auto result = axon::ccr_make_recoverable_output(*db, "shell_filter", "filter.test:stdin",
                                                    original, lossy, 1000);

    ASSERT_TRUE(result.recoverable);
    EXPECT_FALSE(result.artifact_id.empty());
    EXPECT_NE(result.output.find("axon:ccr"), std::string::npos);
    EXPECT_NE(result.output.find(lossy), std::string::npos);
    EXPECT_LT(result.output_tokens, result.input_tokens);
    EXPECT_GT(result.tokens_saved, 0);

    auto artifact = axon::ccr_retrieve_artifact(*db, result.artifact_id);
    ASSERT_TRUE(artifact.has_value());
    EXPECT_EQ(artifact->kind, "shell_filter");
    EXPECT_EQ(artifact->source_ref, "filter.test:stdin");
    EXPECT_EQ(artifact->content, original);
}

TEST_F(CcrTest, RecoverableOutputFallsBackWhenMarkerRemovesSavings) {
    std::string original = "short original output\n";
    std::string lossy = "short summary\n";

    auto result = axon::ccr_make_recoverable_output(*db, "shell_filter", "filter.test:stdin",
                                                    original, lossy, 6);

    EXPECT_FALSE(result.recoverable);
    EXPECT_TRUE(result.artifact_id.empty());
    EXPECT_EQ(result.output, original);
    EXPECT_EQ(result.output_tokens, result.input_tokens);
    EXPECT_EQ(result.tokens_saved, 0);
}

class CcrFileStoreTest : public ::testing::Test {
protected:
    fs::path ccr_dir;

    void SetUp() override {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        ccr_dir = fs::temp_directory_path() / ("axon_ccr_file_test_" + std::to_string(stamp));
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(ccr_dir, ec);
    }
};

TEST_F(CcrFileStoreTest, FileStoreRoundTripsAndMatchesDbArtifactId) {
    std::string content = "int f() {\n    return 42;\n}\n";
    std::string id =
        axon::ccr_store_artifact_file(ccr_dir, "shell_filter", "filter.grep:stdin", content, 12);

    ASSERT_FALSE(id.empty());
    // Same inputs must produce the same id as the DB store, so an artifact
    // is addressable regardless of which store received it.
    EXPECT_EQ(id, axon::ccr_artifact_id("shell_filter", "filter.grep:stdin", content));

    auto artifact = axon::ccr_retrieve_artifact_file(ccr_dir, id);
    ASSERT_TRUE(artifact.has_value());
    EXPECT_EQ(artifact->artifact_id, id);
    EXPECT_EQ(artifact->kind, "shell_filter");
    EXPECT_EQ(artifact->source_ref, "filter.grep:stdin");
    EXPECT_EQ(artifact->content, content);
    EXPECT_EQ(artifact->token_estimate, 12);
}

TEST_F(CcrFileStoreTest, FileStoreMissingAndTraversalIdsReturnNullopt) {
    EXPECT_FALSE(axon::ccr_retrieve_artifact_file(ccr_dir, "ccr_missing").has_value());
    EXPECT_FALSE(axon::ccr_retrieve_artifact_file(ccr_dir, "../etc/passwd").has_value());
    EXPECT_FALSE(axon::ccr_retrieve_artifact_file(ccr_dir, "").has_value());
}

TEST_F(CcrFileStoreTest, RecoverableOutputWorksWithFileStore) {
    std::string original(4000, 'x');
    std::string lossy = "# summary\nimportant line\n";

    axon::CcrStoreFn store = [this](const std::string& k, const std::string& ref,
                                    const std::string& content, int64_t tokens) {
        return axon::ccr_store_artifact_file(ccr_dir, k, ref, content, tokens);
    };
    auto result = axon::ccr_make_recoverable_output(store, "shell_filter", "filter.test:stdin",
                                                    original, lossy, 1000);

    ASSERT_TRUE(result.recoverable);
    EXPECT_FALSE(result.artifact_id.empty());
    EXPECT_NE(result.output.find("axon:ccr"), std::string::npos);
    EXPECT_GT(result.tokens_saved, 0);

    auto artifact = axon::ccr_retrieve_artifact_file(ccr_dir, result.artifact_id);
    ASSERT_TRUE(artifact.has_value());
    EXPECT_EQ(artifact->content, original);
}
