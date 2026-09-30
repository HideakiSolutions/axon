#pragma once

#include <duckdb.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace axon {

// A contract is evidence for one endpoint, never a guessed cross-repository edge.
// `identity` is fully qualified for the surface; an empty identity is unresolved.
struct ContractEvidence {
    std::string repository;
    std::string file;
    std::string symbol;
    std::string surface;  // http, openapi, grpc, topic
    std::string role;     // provider, consumer, declaration
    std::string identity;
    std::string origin;   // observed, declared
    std::string ambiguity; // resolved, unknown
    std::string detail;
};

struct ContractLink {
    ContractEvidence provider;
    ContractEvidence consumer;
    std::string identity;
    std::string resolution; // exact or declaration_resolved
};

// Pass evidence from every registered repository. A suffix-only RPC client is
// resolved only if exactly one declared service/method has that suffix.
std::vector<ContractLink> resolve_contract_links(
    const std::vector<ContractEvidence>& all_evidence);

// Restricted static extraction. Dynamic URL, RPC and topic expressions produce
// unknown evidence; they can never form a confirmed link.
std::vector<ContractEvidence> extract_contracts(const std::filesystem::path& root,
                                                const std::string& repository);

void ensure_contract_schema(duckdb::Connection& conn);
// Returns true only when the indexed evidence actually changed.
bool replace_contract_evidence(duckdb::Connection& conn,
                               const std::vector<ContractEvidence>& evidence);

} // namespace axon
