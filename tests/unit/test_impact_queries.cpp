#include "core/impact_queries.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace {

TEST(ApiShapeQuery, LabeledCompatibleProvenPossibleAndUnknownCases) {
    axon::ResponseShape closed;
    closed.link_identity = "orders|GET|/v1/orders/42";
    closed.provider_symbol = "orders/api.ts::getOrder";
    closed.consumer_symbol = "billing/client.py::readOrder";
    closed.guaranteed_fields = {{"id", "observed"}, {"status", "observed"}};
    closed.accesses = {{"id", "observed", true, false},
                       {"amount", "observed", true, false},
                       {"discount", "observed", false, false},
                       {"", "observed", true, true}};
    closed.closed = true;
    closed.closure_origin = "observed";
    auto findings = axon::compare_response_fields(closed);
    ASSERT_EQ(findings.size(), 4u);
    EXPECT_EQ(findings[0].verdict, "unknown");    // dynamic lookup
    EXPECT_EQ(findings[1].verdict, "proven");     // required field absent from closed shape
    EXPECT_EQ(findings[2].verdict, "possible");   // guarded optional lookup
    EXPECT_EQ(findings[3].verdict, "compatible"); // guaranteed field

    closed.closed = false; // e.g. handler has res.json(variable) on another branch
    findings = axon::compare_response_fields(closed);
    EXPECT_EQ(findings[1].verdict, "possible");
    closed.link_identity.clear(); // no confirmed qualified HTTP link
    findings = axon::compare_response_fields(closed);
    for (const auto& finding : findings)
        EXPECT_EQ(finding.verdict, "unknown");
}

TEST(SymbolCommunityQuery, DeterministicResolvedComponentsAndIsolates) {
    const std::vector<std::string> symbols = {"output", "handler", "client", "alone"};
    std::vector<axon::SymbolRelation> links = {{"handler", "client", "calls", true},
                                               {"client", "output", "imports", true},
                                               {"handler", "output", "calls", false},
                                               {"alone", "handler", "guessed", true}};
    auto first = axon::group_symbol_communities(symbols, links);
    std::reverse(links.begin(), links.end());
    auto second = axon::group_symbol_communities(symbols, links);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0].id, "alone");
    EXPECT_EQ(first[0].symbols.size(), 1u);
    EXPECT_EQ(first[1].symbols, (std::vector<std::string>{"client", "handler", "output"}));
    EXPECT_EQ(first[1].relation_count, 2u);
    EXPECT_EQ(first[1].symbols, second[1].symbols);
    EXPECT_EQ(first[1].relation_count, second[1].relation_count);
}

TEST(ExecutionPathQuery, OrderedBoundedPathsRetainUncertainEdges) {
    const std::vector<axon::ExecutionNode> nodes = {
        {"a_http", "http_handler", ""},       {"b_rpc", "rpc_handler", ""},
        {"c_event", "event_handler", ""},     {"d_call", "call", ""},
        {"e_store", "output", "persistence"}, {"f_http", "output", "external_http"}};
    const std::vector<axon::ExecutionEdge> links = {
        {"a_http", "d_call", "calls", "observed"},
        {"d_call", "e_store", "writes", "inferred"},
        {"d_call", "f_http", "calls", "observed"},
        {"d_call", "a_http", "calls", "observed"}, // cycle
        {"b_rpc", "d_call", "calls", "declared"},
        {"c_event", "f_http", "calls", "observed"}};
    auto trace = axon::trace_execution_paths(nodes, links);
    ASSERT_EQ(trace.paths.size(), 5u);
    EXPECT_EQ(trace.paths[0].nodes, (std::vector<std::string>{"a_http", "d_call", "e_store"}));
    EXPECT_EQ(trace.paths[0].uncertain_edges, (std::vector<bool>{false, true}));
    EXPECT_EQ(trace.paths[0].edge_evidence, (std::vector<std::string>{"observed", "inferred"}));
    EXPECT_EQ(trace.paths[0].output_kind, "persistence");
    EXPECT_FALSE(trace.truncated);
    bool declared_uncertain = false;
    for (const auto& path : trace.paths)
        if (path.nodes.front() == "b_rpc")
            declared_uncertain |=
                path.edge_evidence.front() == "declared" && path.uncertain_edges.front();
    EXPECT_TRUE(declared_uncertain);

    trace = axon::trace_execution_paths(nodes, links, {1, 2, 2});
    EXPECT_TRUE(trace.truncated);
    EXPECT_LE(trace.paths.size(), 2u);
    for (const auto& path : trace.paths)
        EXPECT_LE(path.edge_kinds.size(), 1u);
}

} // namespace
