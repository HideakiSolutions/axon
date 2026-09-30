#include "core/db.hpp"
#include "core/impact_query_adapter.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>

namespace {
namespace fs = std::filesystem;

class ImpactAdapterTest : public ::testing::Test {
protected:
    fs::path root;
    std::unique_ptr<axon::Database> db;

    void SetUp() override {
        static int sequence = 0;
        root = fs::temp_directory_path() /
               ("axon_impact_adapter_" + std::to_string(testsupport::pid()) + "_" +
                std::to_string(++sequence));
        fs::create_directories(root / "src");
        db = std::make_unique<axon::Database>(root / "index.duckdb");
    }

    void TearDown() override {
        db.reset();
        fs::remove_all(root);
    }

    int64_t add_symbol(const std::string& path, const std::string& name,
                       const std::string& source) {
        std::ofstream(root / path) << source;
        int lines = 1;
        for (char ch : source)
            if (ch == '\n') ++lines;
        auto file = db->conn().Query("INSERT INTO files(id,path,language,hash,byte_size) VALUES "
                                     "(nextval('seq_id'), '" +
                                     path + "', 'typescript', 'hash', " +
                                     std::to_string(source.size()) + ") RETURNING id");
        const auto file_id = file->GetValue<int64_t>(0, 0);
        auto symbol =
            db->conn().Query("INSERT INTO symbols(id,file_id,name,kind,start_line,end_line) VALUES "
                             "(nextval('seq_id'), " +
                             std::to_string(file_id) + ", '" + name + "', 'function', 1, " +
                             std::to_string(lines) + ") RETURNING id");
        return symbol->GetValue<int64_t>(0, 0);
    }
};

TEST_F(ImpactAdapterTest, TwoStaticResponsesMustAgreeBeforeAFieldIsGuaranteed) {
    add_symbol("src/api.ts", "getOrder",
               "function getOrder(req, res) {\n"
               " if (req.fast) res.json({id: 1, status: 'ready'});\n"
               " else res.json({id: 2, phase: 'queued'});\n"
               "}\n");
    add_symbol("src/client.ts", "readOrder",
               "function readOrder(response) {\n"
               " console.log(response.data.id);\n"
               " return response.data.amount;\n"
               "}\n");
    const auto index = axon::load_impact_index(db->conn(), root, "orders");
    ASSERT_EQ(index.symbols.size(), 2u);
    axon::ContractLink link;
    link.identity = "orders|GET|/v1/orders";
    link.provider = {"orders",      "src/api.ts", "getOrder", "http", "provider",
                     link.identity, "observed",   "resolved", ""};
    link.consumer = {"orders",      "src/client.ts", "readOrder", "http", "consumer",
                     link.identity, "observed",      "resolved",  ""};
    auto shape = axon::hydrate_http_response_shape(link, index, index);
    ASSERT_TRUE(shape.closed);
    ASSERT_EQ(shape.guaranteed_fields.size(), 1u);
    EXPECT_EQ(shape.guaranteed_fields.front().path, "id");
    auto findings = axon::compare_response_fields(shape);
    ASSERT_EQ(findings.size(), 2u);
    EXPECT_EQ(findings[0].verdict, "proven"); // amount absent from both closed replies
    EXPECT_EQ(findings[1].verdict, "compatible");

    // The provider returns status on only one branch. Its absence cannot be
    // proven from the intersection of guaranteed fields.
    auto conditional_shape = shape;
    conditional_shape.accesses = {{"status", "observed", true, false}};
    const auto conditional_findings = axon::compare_response_fields(conditional_shape);
    ASSERT_EQ(conditional_findings.size(), 1u);
    EXPECT_EQ(conditional_findings.front().verdict, "possible");

    link.identity.clear();
    shape = axon::hydrate_http_response_shape(link, index, index);
    for (const auto& finding : axon::compare_response_fields(shape))
        EXPECT_EQ(finding.verdict, "unknown");
}

TEST_F(ImpactAdapterTest, SpreadAndDynamicResponsesKeepShapeOpen) {
    add_symbol("src/api.ts", "getOrder",
               "function getOrder(req, res) {\n"
               " res.json({id: 1, ...req.body});\n"
               " res.json(req.payload);\n"
               "}\n");
    add_symbol("src/client.ts", "readOrder",
               "function readOrder(response) { return response.data.amount; }\n");
    const auto index = axon::load_impact_index(db->conn(), root, "orders");
    axon::ContractLink link;
    link.identity = "orders|GET|/v1/orders";
    link.provider = {"orders",      "src/api.ts", "getOrder", "http", "provider",
                     link.identity, "observed",   "resolved", ""};
    link.consumer = {"orders",      "src/client.ts", "readOrder", "http", "consumer",
                     link.identity, "observed",      "resolved",  ""};
    auto shape = axon::hydrate_http_response_shape(link, index, index);
    EXPECT_FALSE(shape.closed);
    const auto findings = axon::compare_response_fields(shape);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_NE(findings.front().verdict, "proven");
}

TEST_F(ImpactAdapterTest, IndexedCallsAndQualifiedEntrypointsProduceBoundedTraces) {
    const auto handler = add_symbol("src/http.ts", "handle", "res.json({ok: true});\n");
    const auto worker = add_symbol("src/work.ts", "work", "save(item);\n");
    const auto rpc = add_symbol("src/rpc.ts", "charge", "fetch(url);\n");
    const auto event = add_symbol("src/event.ts", "consume", "emit(event);\n");
    db->conn().Query("INSERT INTO edges(id,from_file,to_file,from_symbol,to_symbol,kind) "
                     "SELECT nextval('seq_id'), a.file_id,b.file_id,a.id,b.id,'calls' "
                     "FROM symbols a,symbols b WHERE a.name='handle' AND b.name='work'");
    const auto index = axon::load_impact_index(db->conn(), root, "orders");
    std::vector<axon::ContractEvidence> evidence = {
        {"orders", "src/http.ts", "handle", "http", "provider", "orders|GET|/x", "observed",
         "resolved", ""},
        {"orders", "src/rpc.ts", "charge", "grpc", "provider", "p|S|Charge", "observed", "resolved",
         ""},
        {"orders", "src/event.ts", "consume", "topic", "consumer", "kafka|a|x", "observed",
         "resolved", ""}};
    auto graph = axon::hydrate_execution_graph(index, evidence);
    auto trace = axon::trace_execution_paths(graph.nodes, graph.edges);
    ASSERT_GE(trace.paths.size(), 4u);
    bool found_persistence = false, found_rpc = false, found_event = false;
    for (const auto& path : trace.paths) {
        if (path.output_kind == "persistence" &&
            path.nodes.front() == "orders:" + std::to_string(handler)) {
            found_persistence = true;
            EXPECT_EQ(path.nodes[1], "orders:" + std::to_string(worker));
            EXPECT_TRUE(path.uncertain_edges.back());
        }
        found_rpc |= path.nodes.front() == "orders:" + std::to_string(rpc);
        found_event |= path.nodes.front() == "orders:" + std::to_string(event);
    }
    EXPECT_TRUE(found_persistence);
    EXPECT_TRUE(found_rpc);
    EXPECT_TRUE(found_event);
    auto bounded = axon::trace_execution_paths(graph.nodes, graph.edges, {1, 1, 1});
    EXPECT_TRUE(bounded.truncated);
    EXPECT_LE(bounded.paths.size(), 1u);
}

} // namespace
