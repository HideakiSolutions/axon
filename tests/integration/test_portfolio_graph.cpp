#include "portfolio/infrastructure/falkordb/falkordb_capability_graph.hpp"
#include <gtest/gtest.h>
#include <algorithm>

namespace {
axon::portfolio::CapabilitySignature signature(const std::string& repo, const std::string& stream, const std::string& id) {
    axon::portfolio::CapabilitySignature value;
    value.signature_id = id; value.stream = {repo, stream}; value.index_epoch = "epoch_1";
    return value;
}
TEST(FalkorDbCapabilityGraph, ReplaceIsolatedAndBounded) {
    axon::portfolio::FalkorDbCapabilityGraph graph("127.0.0.1", 6379, "axon_portfolio_g10_test");
    const axon::portfolio::RepositoryStreamKey first{"11111111-1111-1111-1111-111111111111", "stream_one"};
    const axon::portfolio::RepositoryStreamKey second{"22222222-2222-2222-2222-222222222222", "stream_two"};
    const axon::portfolio::RepositoryStreamKey sibling{first.repository_id, "stream_sibling"};
    EXPECT_NE(axon::portfolio::graph_capability_id(first, "bbbbbbbbbbbbbbbb"),
              axon::portfolio::graph_capability_id(sibling, "bbbbbbbbbbbbbbbb"));
    auto a = signature(first.repository_id, first.index_stream_id, "aaaaaaaaaaaaaaaa");
    auto b = signature(first.repository_id, first.index_stream_id, "bbbbbbbbbbbbbbbb");
    auto c = signature(first.repository_id, first.index_stream_id, "dddddddddddddddd");
    a.call_graph_neighborhood.push_back({"outgoing", "calls", "bbbbbbbbbbbbbbbb", 1, std::nullopt});
    // The incoming declaration must reverse the stored edge. It carries all of the
    // relation evidence so a later graph/UI reader can distinguish it from an
    // outgoing, shallow, digest-less edge.
    b.call_graph_neighborhood.push_back({"incoming", "calls:typed/path", "dddddddddddddddd", 2,
                                         "0123456789abcdef"});
    graph.replace_repository(first, "generation_1", {a, b, c});
    graph.replace_repository(second, "generation_1", {signature(second.repository_id, second.index_stream_id, "cccccccccccccccc")});
    graph.replace_repository(sibling, "generation_1", {signature(sibling.repository_id, sibling.index_stream_id, "bbbbbbbbbbbbbbbb")});
    const auto first_nodes = graph.traverse(first, a.signature_id, 2, 10);
    EXPECT_FALSE(first_nodes.truncated); EXPECT_GE(first_nodes.capability_ids.size(), 1u);
    EXPECT_EQ(std::find(first_nodes.capability_ids.begin(), first_nodes.capability_ids.end(),
                        first.repository_id + ":" + first.index_stream_id + ":" + c.signature_id), first_nodes.capability_ids.end());
    const auto bounded = graph.traverse(first, a.signature_id, 2, 1);
    EXPECT_EQ(bounded.capability_ids.size(), 1u);
    EXPECT_TRUE(bounded.truncated);
    auto b_without_edge = signature(first.repository_id, first.index_stream_id, b.signature_id);
    graph.replace_repository(first, "generation_2", {b_without_edge});
    const auto removed = graph.traverse(first, a.signature_id, 1, 10);
    EXPECT_TRUE(removed.capability_ids.empty());
    const auto preserved = graph.traverse(second, "cccccccccccccccc", 1, 10);
    EXPECT_EQ(preserved.capability_ids.size(), 1u);
    const auto sibling_node = graph.traverse(sibling, "bbbbbbbbbbbbbbbb", 1, 10);
    EXPECT_EQ(sibling_node.capability_ids.size(), 1u);
    auto invalid = signature(first.repository_id, "different_stream", "eeeeeeeeeeeeeeee");
    EXPECT_THROW(graph.replace_repository(first, "generation_4", {b_without_edge, invalid}), std::invalid_argument);
    auto invalid_neighbor = b_without_edge;
    invalid_neighbor.call_graph_neighborhood.push_back({"sideways", "calls", "aaaaaaaaaaaaaaaa", 1, std::nullopt});
    EXPECT_THROW(graph.replace_repository(first, "generation_4", {invalid_neighbor}), std::invalid_argument);
    const auto still_active = graph.traverse(first, b.signature_id, 1, 10);
    EXPECT_EQ(still_active.capability_ids.size(), 1u);
    graph.replace_repository(first, "generation_3", {});
    graph.replace_repository(second, "generation_3", {});
    graph.replace_repository(sibling, "generation_3", {});
}
} // namespace
