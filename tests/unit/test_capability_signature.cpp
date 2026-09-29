#include "portfolio/application/extract/capability_signature_extractor.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;
namespace {

std::vector<std::string> split(const std::string& value) {
    std::vector<std::string> values;
    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        if (!item.empty()) values.push_back(item);
    }
    return values;
}

axon::portfolio::CapabilityInput fixture(const fs::path& path) {
    std::ifstream input(path);
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator != std::string::npos)
            fields.emplace(line.substr(0, separator), line.substr(separator + 1));
    }
    axon::portfolio::CapabilityInput capability;
    capability.entity_key = fields.at("entity_key");
    capability.name = fields.at("name");
    capability.language = fields.at("language");
    capability.content_digest = fields.at("content_digest");
    capability.ast_digest = fields.at("ast_digest");
    capability.path = fields.at("path");
    capability.symbol = fields.at("symbol");
    capability.public_symbols = split(fields.at("public_symbols"));
    capability.routes = split(fields.at("routes"));
    capability.contracts = split(fields.at("contracts"));
    capability.events = split(fields.at("events"));
    capability.external_dependencies = split(fields.at("external_dependencies"));
    capability.tests = split(fields.at("tests"));
    capability.evidence.push_back({"symbol", capability.entity_key, capability.path,
                                   capability.symbol, capability.content_digest, 42, "v1", 1, 8});
    return capability;
}

axon::portfolio::CapabilityExtractionRequest request() {
    return {{"7359f9cf-c2e0-4a61-ab7b-a5fd0918cbbb", "b16e9e79-4a7d-4711-a947-4d3c0d9a7fbb"},
            42,
            "11111111111111111111111111111111",
            "22222222222222222222222222222222",
            "2026-08-31T08:00:00Z",
            "commerce",
            "33333333333333333333333333333333"};
}

fs::path fixture_root() {
    return fs::path(AXON_SOURCE_DIR) / "tests/fixtures/portfolio";
}

} // namespace

TEST(CapabilitySignature, GoldenFixturesAreDeterministicAcrossCppTypeScriptAndPython) {
    auto extraction = request();
    extraction.entities = {fixture(fixture_root() / "cpp-payment.fixture"),
                           fixture(fixture_root() / "ts-catalog.fixture"),
                           fixture(fixture_root() / "python-notification.fixture")};
    axon::portfolio::CapabilitySignatureExtractor extractor;
    const auto first = extractor.extract(extraction);
    const auto second = extractor.extract(extraction);

    ASSERT_EQ(first, second);
    ASSERT_EQ(first.size(), 3u);
    EXPECT_EQ(first[0].signature_id.size(), 64u);
    EXPECT_EQ(first[0].schema_version, "axon/capability-signature/v1");
    EXPECT_NE(first[0].deterministic_summary.find("symbols="), std::string::npos);
    for (const auto& signature : first) {
        EXPECT_FALSE(signature.evidence.empty());
        EXPECT_EQ(signature.evidence.back().kind, "symbol");
        EXPECT_TRUE(signature.ast_fingerprints.empty() ||
                    signature.ast_fingerprints.front().size() >= 16u);
    }
}

TEST(CapabilitySignature, RecomputesOnlyAffectedMetadataPartitions) {
    auto extraction = request();
    extraction.entities = {fixture(fixture_root() / "cpp-payment.fixture"),
                           fixture(fixture_root() / "ts-catalog.fixture"),
                           fixture(fixture_root() / "python-notification.fixture")};
    extraction.affected_entity_keys = {"ts:catalogSearch"};
    extraction.incremental = true;
    const auto signatures = axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction);

    ASSERT_EQ(signatures.size(), 1u);
    EXPECT_EQ(signatures.front().normalized_name, "catalog search");
    EXPECT_EQ(signatures.front().technologies, std::vector<std::string>{"typescript"});
}

TEST(CapabilitySignature, RejectsMissingProvenanceAndNeverAcceptsSourceBodies) {
    auto extraction = request();
    auto input = fixture(fixture_root() / "cpp-payment.fixture");
    input.evidence.clear();
    extraction.entities = {input};
    EXPECT_THROW(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction),
                 std::invalid_argument);

    extraction.entities = {fixture(fixture_root() / "cpp-payment.fixture")};
    extraction.configuration_digest.clear();
    EXPECT_THROW(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction),
                 std::invalid_argument);
}

TEST(CapabilitySignature, CanonicalizesEvidenceAndSkipsInvalidUnaffectedPartitions) {
    auto extraction = request();
    auto first = fixture(fixture_root() / "cpp-payment.fixture");
    auto second = first;
    first.evidence.push_back({"symbol", first.entity_key, "a.cpp", "alpha",
                              "99999999999999999999999999999999", 42, "v1", 2, 2});
    second.evidence.push_back({"symbol", second.entity_key, "z.cpp", "omega",
                               "99999999999999999999999999999999", 42, "v1", 2, 2});
    extraction.entities = {first, second};
    auto reversed = extraction;
    std::swap(reversed.entities[0].evidence, reversed.entities[1].evidence);
    const auto left = axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction);
    const auto right = axon::portfolio::CapabilitySignatureExtractor{}.extract(reversed);
    EXPECT_EQ(left, right);

    auto invalid = fixture(fixture_root() / "python-notification.fixture");
    invalid.evidence.front().kind = "source-body";
    extraction.entities = {fixture(fixture_root() / "ts-catalog.fixture"), invalid};
    extraction.affected_entity_keys = {"ts:catalogSearch"};
    extraction.incremental = true;
    EXPECT_EQ(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction).size(), 1u);
}

TEST(CapabilitySignature, DomainSeparatesAdjacentFingerprintChannels) {
    auto extraction = request();
    auto first = fixture(fixture_root() / "cpp-payment.fixture");
    auto second = first;
    first.events = {"same-token-a"};
    first.internal_dependencies = {"same-token-b"};
    second.events.clear();
    second.internal_dependencies = {"same-token-a", "same-token-b"};
    extraction.entities = {first};
    const auto left = axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction);
    extraction.entities = {second};
    const auto right = axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction);
    ASSERT_EQ(left.size(), 1u);
    ASSERT_EQ(right.size(), 1u);
    EXPECT_NE(left.front().signature_id, right.front().signature_id);
}

TEST(CapabilitySignature, DomainSeparatesAbsentAndEmptyOptionalMetadata) {
    auto extraction = request();
    auto first = fixture(fixture_root() / "cpp-payment.fixture");
    auto second = first;
    extraction.bounded_context.reset();
    extraction.entities = {first};
    const auto absent_context = axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction);
    extraction.bounded_context = "";
    const auto empty_context = axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction);
    ASSERT_EQ(absent_context.size(), 1u);
    ASSERT_EQ(empty_context.size(), 1u);
    EXPECT_NE(absent_context.front().signature_id, empty_context.front().signature_id);

    extraction = request();
    first.evidence.front().path.reset();
    second.evidence.front().path = "";
    extraction.entities = {first};
    const auto absent_path = axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction);
    extraction.entities = {second};
    const auto empty_path = axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction);
    EXPECT_NE(absent_path.front().signature_id, empty_path.front().signature_id);
}

TEST(CapabilitySignature, PreservesAndFingerprintsAllV1OptionalMetadataChannels) {
    auto extraction = request();
    auto input = fixture(fixture_root() / "ts-catalog.fixture");
    input.module = "catalog";
    input.name_space = "axon::catalog";
    input.handlers = {"CatalogSearchHandler"};
    input.schemas = {"CatalogSearchResult"};
    input.dtos = {"CatalogSearchDto"};
    input.call_graph_neighborhood = {
        {"outgoing", "calls", "search-client.query", 1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}};
    input.embedding = {"local-test-model", 384, "cosine", "l2", "qdrant:catalog-search"};
    extraction.entities = {input};
    const auto first = axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first.front().module, "catalog");
    EXPECT_EQ(first.front().name_space, "axon::catalog");
    EXPECT_EQ(first.front().handlers, std::vector<std::string>{"CatalogSearchHandler"});
    ASSERT_TRUE(first.front().embedding.has_value());
    EXPECT_EQ(first.front().embedding->dimension, 384u);

    input.handlers = {"DifferentHandler"};
    extraction.entities = {input};
    const auto changed = axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction);
    EXPECT_NE(first.front().signature_id, changed.front().signature_id);

    auto reversed = input;
    reversed.call_graph_neighborhood = {
        {"incoming", "consumes", "api.request", 2, std::nullopt},
        {"outgoing", "calls", "search-client.query", 1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}};
    input.call_graph_neighborhood = {reversed.call_graph_neighborhood[1],
                                     reversed.call_graph_neighborhood[0]};
    extraction.entities = {input};
    const auto ordered = axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction);
    extraction.entities = {reversed};
    const auto reverse_ordered =
        axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction);
    EXPECT_EQ(ordered, reverse_ordered);
}

TEST(CapabilitySignature, RejectsSchemaViolatingMetadataEvidenceAndEmptyIncrementalImpact) {
    auto extraction = request();
    auto input = fixture(fixture_root() / "cpp-payment.fixture");
    input.name.assign(513, 'x');
    extraction.entities = {input};
    EXPECT_THROW(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction),
                 std::invalid_argument);

    input = fixture(fixture_root() / "cpp-payment.fixture");
    input.evidence.front().kind = "source-body";
    extraction.entities = {input};
    EXPECT_THROW(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction),
                 std::invalid_argument);

    extraction.entities = {fixture(fixture_root() / "cpp-payment.fixture")};
    extraction.affected_entity_keys.clear();
    extraction.incremental = true;
    EXPECT_TRUE(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction).empty());
}

TEST(CapabilitySignature, RejectsOversizedCollectionsAndProvenanceBeforeProjection) {
    auto extraction = request();
    auto input = fixture(fixture_root() / "cpp-payment.fixture");
    input.public_symbols = {std::string(1025, 's')};
    extraction.entities = {input};
    EXPECT_THROW(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction),
                 std::invalid_argument);

    input = fixture(fixture_root() / "cpp-payment.fixture");
    input.routes.assign(1001, "GET /bounded");
    extraction.entities = {input};
    EXPECT_THROW(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction),
                 std::invalid_argument);

    input = fixture(fixture_root() / "cpp-payment.fixture");
    input.ast_digest = "x";
    extraction.entities = {input};
    EXPECT_THROW(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction),
                 std::invalid_argument);

    extraction.entities = {fixture(fixture_root() / "cpp-payment.fixture")};
    extraction.configuration_digest = "x";
    EXPECT_THROW(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction),
                 std::invalid_argument);

    extraction = request();
    extraction.entities = {fixture(fixture_root() / "cpp-payment.fixture")};
    extraction.extracted_at = "not-a-timestamp";
    EXPECT_THROW(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction),
                 std::invalid_argument);

    extraction = request();
    extraction.entities = {fixture(fixture_root() / "cpp-payment.fixture")};
    extraction.extracted_at = "2026-99-99T25:60:60Z";
    EXPECT_THROW(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction),
                 std::invalid_argument);

    extraction = request();
    extraction.entities = {fixture(fixture_root() / "cpp-payment.fixture")};
    extraction.extracted_at = "2026-08-31T08:00:00.Z";
    EXPECT_THROW(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction),
                 std::invalid_argument);

    extraction = request();
    input = fixture(fixture_root() / "cpp-payment.fixture");
    input.name.clear();
    for (int count = 0; count < 256; ++count)
        input.name += "aA";
    extraction.entities = {input};
    EXPECT_THROW(axon::portfolio::CapabilitySignatureExtractor{}.extract(extraction),
                 std::invalid_argument);
}
