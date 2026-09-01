#include "portfolio/application/candidates/capability_candidates.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace {
using namespace axon::portfolio;

CapabilitySignature capability(const std::string& repository, const std::string& stream,
                               const std::string& id,
                               const std::string& name = "payment authorize") {
    CapabilitySignature value;
    value.signature_id = id;
    value.stream = {repository, stream};
    value.index_epoch = "epoch_" + id;
    value.normalized_name = name;
    value.bounded_context = "billing";
    return value;
}

const CapabilityCandidate& only(const std::vector<CapabilityCandidate>& candidates) {
    EXPECT_EQ(candidates.size(), 1u);
    return candidates.front();
}

TEST(CapabilityCandidates, ExactDuplicateRequiresIndependentEvidence) {
    auto left = capability("repo_one", "stream_one", "left");
    auto right = capability("repo_two", "stream_two", "right");
    for (auto* value : {&left, &right}) {
        value->contracts = {"AuthorizeRequest"};
        value->routes = {"POST /authorize"};
        value->events = {"payment.authorized"};
        value->ast_fingerprints = {"ast_a"};
        value->tests = {"authorize_contract"};
    }
    const auto candidate = only(CapabilityCandidateGenerator().generate({left, right}));
    EXPECT_EQ(candidate.classification, CapabilityClassification::exact_duplicate);
    EXPECT_EQ(candidate.recommendation, CapabilityRecommendation::consolidation_review);
    EXPECT_GT(candidate.final_score, 0.0);
    EXPECT_LE(candidate.confidence, 1.0);
    EXPECT_FALSE(candidate.matching_references.empty());
}

TEST(CapabilityCandidates, ClassifiesConvergenceSharedPrimitiveAndSpecialization) {
    auto convergent_left =
        capability("repo_one", "stream_one", "convergent_left", "invoice create");
    auto convergent_right =
        capability("repo_two", "stream_two", "convergent_right", "invoice create");
    convergent_left.contracts = convergent_right.contracts = {"InvoiceRequest"};
    convergent_left.routes = convergent_right.routes = {"POST /invoice"};
    convergent_left.events = convergent_right.events = {"invoice.created"};
    EXPECT_EQ(only(CapabilityCandidateGenerator().generate({convergent_left, convergent_right}))
                  .classification,
              CapabilityClassification::convergent_capability);

    auto primitive_left = capability("repo_three", "stream_three", "primitive_left", "tenant key");
    auto primitive_right =
        capability("repo_four", "stream_four", "primitive_right", "workspace secret");
    primitive_left.contracts = primitive_right.contracts = {"KeyMaterial"};
    primitive_left.routes = primitive_right.routes = {"POST /key"};
    primitive_left.events = primitive_right.events = {"key.rotated"};
    const auto primitive =
        only(CapabilityCandidateGenerator().generate({primitive_left, primitive_right}));
    EXPECT_EQ(primitive.classification, CapabilityClassification::shared_primitive_candidate);
    EXPECT_EQ(primitive.recommendation, CapabilityRecommendation::extract_shared_primitive);

    auto local_left = capability("repo_five", "stream_five", "local_left", "order policy");
    auto local_right = capability("repo_six", "stream_six", "local_right", "workflow policy");
    local_left.contracts = local_right.contracts = {"Policy"};
    local_left.ast_fingerprints = local_right.ast_fingerprints = {"ast_policy"};
    local_left.routes = {"POST /order"};
    local_right.routes = {"POST /workflow"};
    const auto local = only(CapabilityCandidateGenerator().generate({local_left, local_right}));
    EXPECT_EQ(local.classification, CapabilityClassification::local_specialization);
    EXPECT_EQ(local.recommendation, CapabilityRecommendation::keep_local);
}

TEST(CapabilityCandidates, SameNameAcrossDomainsCannotPromoteDuplicate) {
    auto left = capability("repo_one", "stream_one", "left", "status check");
    auto right = capability("repo_two", "stream_two", "right", "status check");
    right.bounded_context = "identity";
    const auto candidate = only(CapabilityCandidateGenerator().generate({left, right}));
    EXPECT_EQ(candidate.classification, CapabilityClassification::semantic_coincidence);
    EXPECT_EQ(candidate.recommendation, CapabilityRecommendation::keep_local);
    EXPECT_LE(candidate.confidence, 0.25);
    EXPECT_NE(std::find(candidate.invalidators.begin(), candidate.invalidators.end(),
                        "bounded_context mismatch prevents duplicate promotion"),
              candidate.invalidators.end());
}

TEST(CapabilityCandidates, DifferentRoutesOrEventsCannotBeAnExactDuplicate) {
    auto left = capability("repo_one", "stream_one", "left", "payment authorize");
    auto right = capability("repo_two", "stream_two", "right", "payment authorize");
    left.contracts = right.contracts = {"AuthorizeRequest"};
    left.ast_fingerprints = right.ast_fingerprints = {"ast_authorize"};
    left.tests = right.tests = {"authorize_contract"};
    left.routes = {"POST /authorize"};
    right.routes = {"POST /authorize/manual-review"};
    left.events = {"payment.authorized"};
    right.events = {"payment.review-requested"};
    const auto candidate = only(CapabilityCandidateGenerator().generate({left, right}));
    EXPECT_EQ(candidate.classification, CapabilityClassification::local_specialization);
    EXPECT_EQ(candidate.recommendation, CapabilityRecommendation::keep_local);
    EXPECT_NE(
        std::find(candidate.differences.begin(), candidate.differences.end(), "routes differ"),
        candidate.differences.end());
    EXPECT_NE(
        std::find(candidate.differences.begin(), candidate.differences.end(), "events differ"),
        candidate.differences.end());
}

TEST(CapabilityCandidates, HandlerDifferenceIsExplainedWhenItCausesSpecialization) {
    auto left = capability("repo_one", "stream_one", "left", "payment authorize");
    auto right = capability("repo_two", "stream_two", "right", "payment authorize");
    left.contracts = right.contracts = {"AuthorizeRequest"};
    left.ast_fingerprints = right.ast_fingerprints = {"ast_authorize"};
    left.tests = right.tests = {"authorize_contract"};
    left.handlers = {"authorize_card"};
    right.handlers = {"authorize_manual_review"};
    const auto candidate = only(CapabilityCandidateGenerator().generate({left, right}));
    EXPECT_EQ(candidate.classification, CapabilityClassification::local_specialization);
    EXPECT_NE(
        std::find(candidate.differences.begin(), candidate.differences.end(), "handlers differ"),
        candidate.differences.end());
}

TEST(CapabilityCandidates, SemanticOnlyRemainsInsufficientAndNoEmbeddingPathWorks) {
    auto left = capability("repo_one", "stream_one", "left", "alpha");
    auto right = capability("repo_two", "stream_two", "right", "beta");
    const auto left_id = capability_reference_id(left);
    const auto right_id = capability_reference_id(right);
    const auto candidate =
        only(CapabilityCandidateGenerator().generate({left, right}, {{left_id, right_id, .95}}));
    EXPECT_EQ(candidate.classification, CapabilityClassification::insufficient_evidence);
    EXPECT_EQ(candidate.recommendation, CapabilityRecommendation::collect_more_evidence);
    EXPECT_LE(candidate.confidence, .40);
    EXPECT_NE(std::find(candidate.invalidators.begin(), candidate.invalidators.end(),
                        "fewer than two independent positive signals"),
              candidate.invalidators.end());
}

TEST(CapabilityCandidates, RankingIsDeterministicBoundedAndSkipsSameRepositoryVariants) {
    auto source = capability("repo_one", "stream_one", "source", "token refresh");
    auto first = capability("repo_two", "stream_two", "first", "token refresh");
    auto second = capability("repo_three", "stream_three", "second", "token refresh");
    auto sibling = capability("repo_one", "stream_sibling", "sibling", "token refresh");
    CandidateGenerationConfig config;
    config.max_candidates_per_capability = 1;
    config.max_candidates = 1;
    const auto first_run =
        CapabilityCandidateGenerator(config).generate({source, second, first, sibling});
    const auto second_run =
        CapabilityCandidateGenerator(config).generate({source, second, first, sibling});
    ASSERT_EQ(first_run.size(), 1u);
    ASSERT_EQ(second_run.size(), 1u);
    EXPECT_EQ(first_run.front().candidate_id, second_run.front().candidate_id);
    EXPECT_EQ(first_run.front().candidate_id.find("stream_sibling"), std::string::npos);
    EXPECT_GE(first_run.front().final_score, 0.0);
    EXPECT_LE(first_run.front().final_score, 1.0);
}

TEST(CapabilityCandidates, FinalFanoutCannotExceedConfiguredPerCapabilityLimit) {
    auto one = capability("repo_one", "stream_one", "one", "token refresh");
    auto two = capability("repo_two", "stream_two", "two", "token refresh");
    auto three = capability("repo_three", "stream_three", "three", "token refresh");
    auto four = capability("repo_four", "stream_four", "four", "token refresh");
    CandidateGenerationConfig config;
    config.max_candidates_per_capability = 1;
    config.max_candidates = 100;
    const auto candidates = CapabilityCandidateGenerator(config).generate({one, two, three, four});
    std::map<std::string, std::size_t> fanout;
    for (const auto& candidate : candidates) {
        ++fanout[candidate.left_capability_id];
        ++fanout[candidate.right_capability_id];
    }
    for (const auto& [capability_id, count] : fanout) {
        (void)capability_id;
        EXPECT_LE(count, 1u);
    }
    EXPECT_LE(candidates.size(), 2u);
}

TEST(CapabilityCandidates, RejectsInvalidHintsAndBounds) {
    CandidateGenerationConfig bad;
    bad.rrf_k = 0;
    EXPECT_THROW((void)CapabilityCandidateGenerator{bad}, std::invalid_argument);
    auto left = capability("repo_one", "stream_one", "left");
    auto right = capability("repo_two", "stream_two", "right");
    EXPECT_THROW(
        CapabilityCandidateGenerator().generate({left, right}, {{"unknown", "also_unknown", .5}}),
        std::invalid_argument);
}

TEST(CapabilityCandidates, MultiSignalOutperformsSingleSignalBaselinesOnTruthFixture) {
    auto duplicate_left = capability("repo_one", "stream_one", "duplicate_left", "session revoke");
    auto duplicate_right =
        capability("repo_two", "stream_two", "duplicate_right", "session revoke");
    for (auto* value : {&duplicate_left, &duplicate_right}) {
        value->contracts = {"RevokeRequest"};
        value->routes = {"POST /revoke"};
        value->events = {"session.revoked"};
        value->ast_fingerprints = {"ast_revoke"};
    }
    CandidateGenerationConfig name_only;
    name_only.weights = {{CandidateSignal::name, 1.0}};
    const auto name_baseline =
        only(CapabilityCandidateGenerator(name_only).generate({duplicate_left, duplicate_right}));
    EXPECT_EQ(name_baseline.classification, CapabilityClassification::insufficient_evidence);
    const auto left_id = capability_reference_id(duplicate_left);
    const auto right_id = capability_reference_id(duplicate_right);
    CandidateGenerationConfig semantic_only;
    semantic_only.weights = {{CandidateSignal::semantic, 1.0}};
    const auto semantic_baseline =
        only(CapabilityCandidateGenerator(semantic_only)
                 .generate({duplicate_left, duplicate_right}, {{left_id, right_id, .99}}));
    EXPECT_EQ(semantic_baseline.classification, CapabilityClassification::insufficient_evidence);
    const auto multi_signal = only(CapabilityCandidateGenerator().generate(
        {duplicate_left, duplicate_right}, {{left_id, right_id, .99}}));
    EXPECT_EQ(multi_signal.classification, CapabilityClassification::exact_duplicate);
    EXPECT_EQ(name_baseline.recommendation, CapabilityRecommendation::collect_more_evidence);
    EXPECT_EQ(semantic_baseline.recommendation, CapabilityRecommendation::collect_more_evidence);
    const auto promoted = [](const CapabilityCandidate& candidate) {
        return candidate.classification == CapabilityClassification::exact_duplicate ||
               candidate.classification == CapabilityClassification::convergent_capability ||
               candidate.classification == CapabilityClassification::shared_primitive_candidate;
    };
    // One labelled positive at K=1 gives a compact, reproducible baseline report:
    // name-only P@1/R@1 = 0/0, semantic-only P@1/R@1 = 0/0, multi-signal P@1/R@1 = 1/1.
    EXPECT_DOUBLE_EQ(promoted(name_baseline) ? 1.0 : 0.0, 0.0);
    EXPECT_DOUBLE_EQ(promoted(semantic_baseline) ? 1.0 : 0.0, 0.0);
    EXPECT_DOUBLE_EQ(promoted(multi_signal) ? 1.0 : 0.0, 1.0);
}

TEST(CapabilityCandidates, VersionedTruthSetHasEveryClassificationAndBaseline) {
    std::ifstream input(std::string(AXON_SOURCE_DIR) + "/evals/portfolio/truth-set-v1.json");
    ASSERT_TRUE(input.good());
    const auto truth_set = nlohmann::json::parse(input);
    EXPECT_EQ(truth_set.at("schema_version"), "axon/portfolio-candidate-truth-set/v1");
    std::set<std::string> classifications;
    for (const auto& test_case : truth_set.at("cases")) {
        classifications.insert(test_case.at("expected").get<std::string>());
    }
    EXPECT_EQ(classifications,
              (std::set<std::string>{"exact_duplicate", "convergent_capability",
                                     "shared_primitive_candidate", "local_specialization",
                                     "semantic_coincidence", "insufficient_evidence"}));
    EXPECT_EQ(truth_set.at("baselines"),
              (std::vector<std::string>{"name-only", "semantic-only", "multi-signal"}));
}
} // namespace
