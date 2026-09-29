#pragma once

#include "portfolio/domain/provider_contract.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace axon::portfolio {

// Metadata-only evidence. Source text is deliberately absent: the local index remains the sole
// authority for code and the central projection receives only verifiable references and digests.
struct CapabilityEvidence {
    std::string kind;
    std::string entity_key;
    std::optional<std::string> path;
    std::optional<std::string> symbol;
    std::string digest;
    std::uint64_t source_sequence = 0;
    std::string extractor_version;
    std::optional<std::uint32_t> line_start;
    std::optional<std::uint32_t> line_end;

    bool operator==(const CapabilityEvidence&) const = default;
};

struct CapabilityNeighbor {
    std::string direction;
    std::string relation;
    std::string entity_key;
    std::uint32_t distance = 0;
    std::optional<std::string> digest;
    bool operator==(const CapabilityNeighbor&) const = default;
};

struct CapabilityEmbedding {
    std::string model_id;
    std::uint32_t dimension = 0;
    std::string metric;
    std::string normalization;
    std::string vector_ref;
    bool operator==(const CapabilityEmbedding&) const = default;
};

struct CapabilityInput {
    std::string entity_key;
    std::string name;
    std::string language;
    std::optional<std::string> module;
    std::optional<std::string> name_space;
    std::string content_digest;
    std::optional<std::string> ast_digest;
    std::optional<std::string> path;
    std::optional<std::string> symbol;
    std::vector<std::string> public_symbols;
    std::vector<std::string> routes;
    std::vector<std::string> handlers;
    std::vector<std::string> contracts;
    std::vector<std::string> events;
    std::vector<std::string> schemas;
    std::vector<std::string> dtos;
    std::vector<std::string> internal_dependencies;
    std::vector<std::string> external_dependencies;
    std::vector<std::string> tests;
    std::vector<CapabilityNeighbor> call_graph_neighborhood;
    std::optional<CapabilityEmbedding> embedding;
    std::vector<CapabilityEvidence> evidence;
};

struct CapabilityProvenance {
    std::string extractor_id = "axon.metadata-capability-extractor";
    std::string extractor_version = "v1";
    std::string source_schema_version = "axon/index-metadata/v1";
    std::string configuration_digest;

    bool operator==(const CapabilityProvenance&) const = default;
};

struct CapabilitySignature {
    std::string schema_version = "axon/capability-signature/v1";
    std::string signature_id;
    RepositoryStreamKey stream;
    std::uint64_t source_sequence = 0;
    std::string index_epoch;
    std::string manifest_hash;
    // Caller-supplied snapshot/event timestamp. The extractor never reads a wall clock.
    std::string extracted_at;
    std::optional<std::string> bounded_context;
    std::optional<std::string> module;
    std::optional<std::string> name_space;
    std::optional<std::string> path;
    std::string normalized_name;
    std::string deterministic_summary;
    std::vector<std::string> public_symbols;
    std::vector<std::string> routes;
    std::vector<std::string> handlers;
    std::vector<std::string> contracts;
    std::vector<std::string> events;
    std::vector<std::string> schemas;
    std::vector<std::string> dtos;
    std::vector<std::string> internal_dependencies;
    std::vector<std::string> external_dependencies;
    std::vector<std::string> tests;
    std::vector<CapabilityNeighbor> call_graph_neighborhood;
    std::optional<CapabilityEmbedding> embedding;
    std::vector<std::string> technologies;
    std::vector<std::string> ast_fingerprints;
    std::vector<CapabilityEvidence> evidence;
    CapabilityProvenance provenance;

    bool operator==(const CapabilitySignature&) const = default;
};

std::string normalize_capability_name(const std::string& name);
std::string capability_fingerprint(const CapabilitySignature& signature);

} // namespace axon::portfolio
