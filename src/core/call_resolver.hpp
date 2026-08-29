#pragma once

#include "../parser/parser.hpp"

#include <duckdb.hpp>

#include <cstdint>
#include <vector>

namespace axon {

// Resolve parsed call sites into kind='calls' symbol edges. Candidates are
// ranked by receiver/owner type, parameter arity, locality and production-file
// preference, with stable-id fallback when semantic hints are unavailable.
int resolve_call_edges(duckdb::Connection& conn, int64_t from_file_id,
                       const std::vector<CallSite>& calls);

} // namespace axon
