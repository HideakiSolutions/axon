#include "core/call_resolver.hpp"
#include "core/db.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace {
namespace fs = std::filesystem;

class CallResolutionTest : public ::testing::Test {
protected:
    fs::path db_path;
    std::unique_ptr<axon::Database> db;

    void SetUp() override {
        static int counter = 0;
        db_path = fs::temp_directory_path() /
                  ("axon_call_resolution_" + std::to_string(testsupport::pid()) + "_" +
                   std::to_string(++counter) + ".duckdb");
        db = std::make_unique<axon::Database>(db_path);
    }

    void TearDown() override {
        db.reset();
        fs::remove(db_path);
    }

    int64_t add_file(const std::string& path) {
        auto result =
            db->conn().Query("INSERT INTO files (id, path, language, hash, byte_size) VALUES "
                             "(nextval('seq_id'), '" +
                             path + "', 'typescript', 'hash', 1) RETURNING id");
        return result->GetValue<int64_t>(0, 0);
    }

    int64_t add_symbol(int64_t file_id, const std::string& name, const std::string& kind, int start,
                       int end, const std::string& signature) {
        auto result = db->conn().Query(
            "INSERT INTO symbols (id, file_id, name, kind, start_line, end_line, signature) "
            "VALUES (nextval('seq_id'), " +
            std::to_string(file_id) + ", '" + name + "', '" + kind + "', " + std::to_string(start) +
            ", " + std::to_string(end) + ", '" + signature + "') RETURNING id");
        return result->GetValue<int64_t>(0, 0);
    }
};

TEST_F(CallResolutionTest, QualifierAndAritySelectTheCorrectOverload) {
    const auto caller_file = add_file("src/caller.ts");
    const auto alpha_file = add_file("src/alpha.ts");
    const auto beta_file = add_file("src/beta.ts");
    const auto caller = add_symbol(caller_file, "invoke", "function", 1, 10, "function invoke()");
    add_symbol(alpha_file, "Alpha", "class", 1, 30, "class Alpha");
    add_symbol(alpha_file, "run", "method", 5, 8, "run(value: string)");
    add_symbol(beta_file, "Beta", "class", 1, 30, "class Beta");
    const auto expected =
        add_symbol(beta_file, "run", "method", 5, 8, "run(left: string, right: string)");

    axon::CallSite call{"invoke", "run", "Beta", 2, 4};
    EXPECT_EQ(axon::resolve_call_edges(db->conn(), caller_file, {call}), 1);

    auto edge = db->conn().Query("SELECT from_symbol, to_symbol FROM edges WHERE kind = 'calls'");
    ASSERT_EQ(edge->RowCount(), 1u);
    EXPECT_EQ(edge->GetValue<int64_t>(0, 0), caller);
    EXPECT_EQ(edge->GetValue<int64_t>(1, 0), expected);
}

TEST_F(CallResolutionTest, ThisReceiverUsesTheEnclosingCallerType) {
    const auto file = add_file("src/service.ts");
    add_symbol(file, "Service", "class", 1, 40, "class Service");
    const auto caller = add_symbol(file, "invoke", "method", 5, 15, "invoke()");
    const auto expected = add_symbol(file, "run", "method", 20, 25, "run(value: string)");

    const auto other_file = add_file("src/other.ts");
    add_symbol(other_file, "Other", "class", 1, 30, "class Other");
    add_symbol(other_file, "run", "method", 5, 8, "run(value: string)");

    axon::CallSite call{"invoke", "run", "this", 1, 10};
    EXPECT_EQ(axon::resolve_call_edges(db->conn(), file, {call}), 1);

    auto edge = db->conn().Query("SELECT from_symbol, to_symbol FROM edges WHERE kind = 'calls'");
    ASSERT_EQ(edge->RowCount(), 1u);
    EXPECT_EQ(edge->GetValue<int64_t>(0, 0), caller);
    EXPECT_EQ(edge->GetValue<int64_t>(1, 0), expected);
}

TEST_F(CallResolutionTest, MissingSemanticHintsKeepSameFileFallback) {
    const auto file = add_file("src/local.ts");
    const auto caller = add_symbol(file, "invoke", "function", 1, 10, "invoke()");
    const auto expected = add_symbol(file, "run", "function", 20, 22, "run(value: string)");
    const auto remote = add_file("src/remote.ts");
    add_symbol(remote, "run", "function", 1, 3, "run(value: string)");

    axon::CallSite call{"invoke", "run", "", -1, 4};
    EXPECT_EQ(axon::resolve_call_edges(db->conn(), file, {call}), 1);

    auto edge = db->conn().Query("SELECT from_symbol, to_symbol FROM edges WHERE kind = 'calls'");
    ASSERT_EQ(edge->RowCount(), 1u);
    EXPECT_EQ(edge->GetValue<int64_t>(0, 0), caller);
    EXPECT_EQ(edge->GetValue<int64_t>(1, 0), expected);
}

TEST_F(CallResolutionTest, ImplicitSelfDoesNotCountAsACallArgument) {
    const auto file = add_file("src/service.py");
    add_symbol(file, "Service", "class", 1, 40, "class Service");
    add_symbol(file, "invoke", "method", 5, 10, "def invoke(self)");
    const auto expected = add_symbol(file, "run", "method", 15, 18, "def run(self)");
    add_symbol(file, "run", "method", 20, 24, "def run(self, value: str)");

    axon::CallSite call{"invoke", "run", "self", 0, 8};
    EXPECT_EQ(axon::resolve_call_edges(db->conn(), file, {call}), 1);

    auto edge = db->conn().Query("SELECT to_symbol FROM edges WHERE kind = 'calls'");
    ASSERT_EQ(edge->RowCount(), 1u);
    EXPECT_EQ(edge->GetValue<int64_t>(0, 0), expected);
}

} // namespace
