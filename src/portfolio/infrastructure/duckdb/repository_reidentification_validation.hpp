#pragma once

#include "portfolio/application/portfolio_store.hpp"

#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>

namespace axon::portfolio::duckdb_detail {

enum class ReidentificationValidationError { None, InvalidInput, CursorConflict };

struct ReidentificationValidation {
    ReidentificationValidationError error = ReidentificationValidationError::None;
    const char* message = "";

    explicit operator bool() const { return error == ReidentificationValidationError::None; }
};

inline bool is_uuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

inline ReidentificationValidation validate_reidentification(
    const RepositoryReidentification& value, std::uint64_t expected_cursor) {
    static const std::unordered_set<std::string> reasons = {
        "contract-adopted", "collision-repaired", "repository-moved", "owner-approved"};
    if (!is_uuid(value.previous_stream.repository_id) ||
        !is_uuid(value.previous_stream.index_stream_id) ||
        !is_uuid(value.current_stream.repository_id) ||
        !is_uuid(value.current_stream.index_stream_id) ||
        value.previous_stream.repository_id == value.current_stream.repository_id ||
        value.previous_stream.index_stream_id != value.current_stream.index_stream_id ||
        value.sequence == 0 || value.event_id.size() < 16 || value.event_id.size() > 128 ||
        value.epoch.size() < 16 || value.epoch.size() > 128 ||
        (value.manifest && (value.manifest->size() < 16 || value.manifest->size() > 128)) ||
        value.old_binding_id.size() < 16 || value.old_binding_id.size() > 128 ||
        value.new_binding_id.size() < 16 || value.new_binding_id.size() > 128 ||
        value.approval_reference.empty() || value.approval_reference.size() > 512 ||
        reasons.count(value.reason) == 0)
        return {ReidentificationValidationError::InvalidInput,
                "invalid repository reidentification bounds"};
    if (expected_cursor == std::numeric_limits<std::uint64_t>::max() ||
        value.sequence != expected_cursor + 1)
        return {ReidentificationValidationError::CursorConflict,
                "repository reidentification sequence is not contiguous"};
    return {};
}

} // namespace axon::portfolio::duckdb_detail
