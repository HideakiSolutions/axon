#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace axon {

struct FusedMemoryRank {
    int64_t observation_id;
    std::optional<int> semantic_rank;
    std::optional<int> lexical_rank;
    double rrf_score;
    double authority;
    double final_score;
};

double bounded_memory_authority(double authority);
std::vector<std::string> memory_lexical_terms(const std::string& query, size_t max_terms = 8);
std::vector<FusedMemoryRank>
fuse_memory_ranks(const std::vector<int64_t>& semantic_ids, const std::vector<int64_t>& lexical_ids,
                  const std::unordered_map<int64_t, double>& authority_by_id, size_t limit,
                  double rrf_k = 60.0);

} // namespace axon
