#pragma once

#include "portfolio/domain/capability_signature.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace axon::portfolio {

enum class CandidateSignal {
    semantic,
    name,
    contracts,
    endpoints,
    events,
    structure,
    dependencies,
    graph_neighborhood,
    tests,
    domain_context
};
enum class CapabilityClassification {
    exact_duplicate,
    convergent_capability,
    shared_primitive_candidate,
    local_specialization,
    semantic_coincidence,
    insufficient_evidence
};
enum class CapabilityRecommendation {
    consolidation_review,
    extract_shared_primitive,
    keep_local,
    collect_more_evidence
};

struct SemanticCandidateHint {
    std::string left_capability_id;
    std::string right_capability_id;
    double score = 0.0;
};
struct CandidateSignalScore {
    CandidateSignal signal = CandidateSignal::name;
    double raw_score = 0.0;
    std::size_t rank = 0;
    double rrf_score = 0.0;
    std::vector<std::string> evidence;
};
struct CapabilityCandidate {
    std::string candidate_id, left_capability_id, right_capability_id, left_repository_id,
        right_repository_id;
    std::string left_index_epoch, right_index_epoch;
    double final_score = 0.0, confidence = 0.0;
    CapabilityClassification classification = CapabilityClassification::insufficient_evidence;
    CapabilityRecommendation recommendation = CapabilityRecommendation::collect_more_evidence;
    std::vector<CandidateSignalScore> signals;
    std::vector<std::string> matching_references, differences, invalidators;
};
struct CandidateGenerationConfig {
    std::size_t rrf_k = 60, max_signatures = 2000, max_candidates_per_capability = 100,
                max_candidates = 10000;
    std::map<CandidateSignal, double> weights;
};
class CapabilityCandidateGenerator {
public:
    explicit CapabilityCandidateGenerator(CandidateGenerationConfig config = {});
    std::vector<CapabilityCandidate>
    generate(const std::vector<CapabilitySignature>& signatures,
             const std::vector<SemanticCandidateHint>& semantic_hints = {}) const;

private:
    CandidateGenerationConfig config_;
};
const char* to_string(CandidateSignal signal);
const char* to_string(CapabilityClassification classification);
const char* to_string(CapabilityRecommendation recommendation);
std::string capability_reference_id(const CapabilitySignature& signature);
} // namespace axon::portfolio
