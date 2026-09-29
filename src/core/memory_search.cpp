#include "memory_search.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

namespace axon {

double bounded_memory_authority(double authority) {
    if (!std::isfinite(authority)) return 1.0;
    return std::clamp(authority, 0.5, 2.0);
}

std::vector<std::string> memory_lexical_terms(const std::string& query, size_t max_terms) {
    std::vector<std::string> terms;
    std::unordered_set<std::string> seen;
    std::string current;

    auto flush = [&]() {
        if (current.size() >= 2 && current.size() <= 64 && seen.insert(current).second)
            terms.push_back(current);
        current.clear();
    };

    for (unsigned char character : query) {
        if (std::isalnum(character) || character == '_' || character == '-' || character == '.' ||
            character == '/') {
            current.push_back(static_cast<char>(std::tolower(character)));
        } else {
            flush();
            if (terms.size() >= max_terms) break;
        }
    }
    if (terms.size() < max_terms) flush();
    if (terms.size() > max_terms) terms.resize(max_terms);
    return terms;
}

std::vector<FusedMemoryRank>
fuse_memory_ranks(const std::vector<int64_t>& semantic_ids, const std::vector<int64_t>& lexical_ids,
                  const std::unordered_map<int64_t, double>& authority_by_id, size_t limit,
                  double rrf_k) {
    if (limit == 0) return {};
    if (!std::isfinite(rrf_k) || rrf_k <= 0.0) rrf_k = 60.0;

    std::unordered_map<int64_t, FusedMemoryRank> fused;
    const auto add_channel = [&](const std::vector<int64_t>& ids, bool semantic) {
        std::unordered_set<int64_t> seen;
        for (size_t index = 0; index < ids.size(); ++index) {
            const auto id = ids[index];
            if (!seen.insert(id).second) continue;
            const int rank = static_cast<int>(index + 1);
            auto [iterator, inserted] = fused.try_emplace(
                id, FusedMemoryRank{id, std::nullopt, std::nullopt, 0.0, 1.0, 0.0});
            auto& candidate = iterator->second;
            if (semantic)
                candidate.semantic_rank = rank;
            else
                candidate.lexical_rank = rank;
            candidate.rrf_score += 1.0 / (rrf_k + static_cast<double>(rank));
        }
    };

    add_channel(semantic_ids, true);
    add_channel(lexical_ids, false);

    std::vector<FusedMemoryRank> ranked;
    ranked.reserve(fused.size());
    for (auto& [id, candidate] : fused) {
        if (const auto authority = authority_by_id.find(id); authority != authority_by_id.end())
            candidate.authority = bounded_memory_authority(authority->second);
        candidate.final_score = candidate.rrf_score * candidate.authority;
        ranked.push_back(candidate);
    }

    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        if (left.final_score != right.final_score) return left.final_score > right.final_score;
        return left.observation_id < right.observation_id;
    });
    if (ranked.size() > limit) ranked.resize(limit);
    return ranked;
}

} // namespace axon
