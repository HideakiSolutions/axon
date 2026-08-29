#include <gtest/gtest.h>

#include "core/memory_search.hpp"

#include <limits>
#include <unordered_map>

TEST(MemorySearch, LexicalTermsAreNormalizedBoundedAndUnique) {
    auto terms = axon::memory_lexical_terms(
        "Session_Start session_start auth/token.cpp X exact-match ninth tenth", 4);
    ASSERT_EQ(terms.size(), 4u);
    EXPECT_EQ(terms[0], "session_start");
    EXPECT_EQ(terms[1], "auth/token.cpp");
    EXPECT_EQ(terms[2], "exact-match");
    EXPECT_EQ(terms[3], "ninth");
}

TEST(MemorySearch, ReciprocalRankFusionRewardsBothChannels) {
    std::vector<int64_t> semantic{10, 20, 30};
    std::vector<int64_t> lexical{30, 40, 10};
    auto ranked = axon::fuse_memory_ranks(semantic, lexical, {}, 10);

    ASSERT_EQ(ranked.size(), 4u);
    EXPECT_EQ(ranked[0].observation_id, 10);
    EXPECT_TRUE(ranked[0].semantic_rank.has_value());
    EXPECT_TRUE(ranked[0].lexical_rank.has_value());
    EXPECT_GT(ranked[0].rrf_score, ranked[2].rrf_score);
}

TEST(MemorySearch, AuthorityIsBoundedAfterFusion) {
    std::vector<int64_t> semantic{1, 2, 3};
    std::unordered_map<int64_t, double> authority{{1, 0.01}, {2, 1.0}, {3, 99.0}};
    auto ranked = axon::fuse_memory_ranks(semantic, {}, authority, 3);

    ASSERT_EQ(ranked.size(), 3u);
    EXPECT_EQ(ranked[0].observation_id, 3);
    EXPECT_DOUBLE_EQ(ranked[0].authority, 2.0);
    EXPECT_EQ(ranked[2].observation_id, 1);
    EXPECT_DOUBLE_EQ(ranked[2].authority, 0.5);
    EXPECT_DOUBLE_EQ(axon::bounded_memory_authority(std::numeric_limits<double>::quiet_NaN()), 1.0);
}

TEST(MemorySearch, DuplicateChannelEntriesUseFirstRankOnly) {
    auto ranked = axon::fuse_memory_ranks({7, 7, 8}, {8}, {}, 10);
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0].observation_id, 8);
    EXPECT_EQ(ranked[1].observation_id, 7);
    EXPECT_EQ(*ranked[1].semantic_rank, 1);
}

TEST(MemorySearch, EqualScoresUseStableObservationIdTieBreak) {
    auto ranked = axon::fuse_memory_ranks({20, 10}, {10, 20}, {}, 10);
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_DOUBLE_EQ(ranked[0].final_score, ranked[1].final_score);
    EXPECT_EQ(ranked[0].observation_id, 10);
    EXPECT_EQ(ranked[1].observation_id, 20);
}
