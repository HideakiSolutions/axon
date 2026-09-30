#include "core/data_trace.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <unordered_map>

#ifndef AXON_SOURCE_DIR
#define AXON_SOURCE_DIR "."
#endif

namespace {
std::filesystem::path fixture(const char* name) {
    return std::filesystem::path(AXON_SOURCE_DIR) / "evals/data_trace" / name;
}
std::unordered_map<std::string, std::string> statuses(const axon::DataTraceResult& result) {
    std::unordered_map<std::string, std::string> out;
    for (const auto& path : result.paths) out[path.function] = path.status;
    return out;
}
}

TEST(DataTraceTest, LabeledTypeScriptAndJavaScriptCorpus) {
    auto ts = axon::trace_data_flow(fixture("corpus.ts"));
    ASSERT_TRUE(ts.supported);
    auto a = statuses(ts);
    EXPECT_EQ(a["saveName"], "confirmed");
    EXPECT_EQ(a["postEmail"], "confirmed");
    EXPECT_EQ(a["saveAfterBranch"], "unknown");
    EXPECT_EQ(a["saveInsideBranch"], "unknown");
    EXPECT_EQ(a["saveInsideElse"], "unknown");
    EXPECT_FALSE(a.count("overwrittenBothBranches"));
    EXPECT_EQ(a["saveDynamic"], "unknown");
    EXPECT_EQ(a["saveHelper"], "unknown");
    EXPECT_FALSE(a.count("saveConstant"));
    EXPECT_FALSE(a.count("unrelatedRequest"));
    ASSERT_GE(ts.paths.size(), 5u);
    EXPECT_EQ(ts.paths.front().steps.front().kind, "source");
    EXPECT_EQ(ts.paths.front().steps.back().kind, "sink");
    EXPECT_GT(ts.elapsed_us, 0);
    EXPECT_GT(ts.trace_bytes, 0u);

    auto js = axon::trace_data_flow(fixture("corpus.js"));
    ASSERT_TRUE(js.supported);
    auto b = statuses(js);
    EXPECT_EQ(b["saveQuery"], "confirmed");
    EXPECT_FALSE(b.count("noFlow"));
}

TEST(DataTraceTest, UnsupportedLanguageAndSymbolFilter) {
    auto unsupported = axon::trace_data_flow(fixture("truth-v1.json"));
    EXPECT_FALSE(unsupported.supported);
    auto filtered = axon::trace_data_flow(fixture("corpus.ts"), "saveName");
    ASSERT_EQ(filtered.paths.size(), 1u);
    EXPECT_EQ(filtered.paths.front().function, "saveName");
}
