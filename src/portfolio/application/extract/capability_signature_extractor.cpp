#include "capability_signature_extractor.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>

namespace axon::portfolio {
namespace {

void canonicalize(std::vector<std::string>& values) {
    values.erase(std::remove_if(values.begin(), values.end(),
                                [](const auto& value) { return value.empty(); }),
                 values.end());
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

bool bounded(const std::string& value, std::size_t minimum, std::size_t maximum) {
    return value.size() >= minimum && value.size() <= maximum;
}

bool uuid(const std::string& value) {
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

bool timestamp(const std::string& value) {
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value.back() != 'Z')
        return false;
    for (const auto index : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u}) {
        if (!std::isdigit(static_cast<unsigned char>(value[index]))) return false;
    }
    if (value.size() > 20) {
        if (value[19] != '.' || value.size() == 21) return false;
        for (std::size_t index = 20; index + 1 < value.size(); ++index) {
            if (!std::isdigit(static_cast<unsigned char>(value[index]))) return false;
        }
    }
    const auto number = [&value](std::size_t start, std::size_t count) {
        return std::stoi(value.substr(start, count));
    };
    const int year = number(0, 4);
    const int month = number(5, 2);
    const int day = number(8, 2);
    const int hour = number(11, 2);
    const int minute = number(14, 2);
    const int second = number(17, 2);
    if (year == 0 || month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59)
        return false;
    static constexpr int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int maximum_day = month == 2 && leap ? 29 : days_per_month[month - 1];
    return day >= 1 && day <= maximum_day;
}

void validate_strings(const std::vector<std::string>& values, std::size_t max_items,
                      std::size_t max_length, const char* field) {
    if (values.size() > max_items)
        throw std::invalid_argument(std::string(field) + " exceeds v1 item limit");
    for (const auto& value : values) {
        if (value.size() > max_length)
            throw std::invalid_argument(std::string(field) + " exceeds v1 value limit");
    }
}

void validate_neighbors(const std::vector<CapabilityNeighbor>& neighbors) {
    static const std::set<std::string> directions = {"incoming", "outgoing"};
    static const std::set<std::string> relations = {"calls",     "implements", "imports",
                                                    "publishes", "consumes",   "depends-on"};
    if (neighbors.size() > 5000)
        throw std::invalid_argument("call graph neighborhood exceeds v1 item limit");
    for (const auto& neighbor : neighbors) {
        if (!directions.contains(neighbor.direction) || !relations.contains(neighbor.relation) ||
            !bounded(neighbor.entity_key, 1, 4096) || neighbor.distance == 0 ||
            neighbor.distance > 8 || (neighbor.digest && !bounded(*neighbor.digest, 16, 128)))
            throw std::invalid_argument("call graph neighborhood violates v1 metadata bounds");
    }
}

void validate_embedding(const std::optional<CapabilityEmbedding>& embedding) {
    if (!embedding) return;
    if (!bounded(embedding->model_id, 1, 512) || embedding->dimension == 0 ||
        embedding->dimension > 65536 ||
        (embedding->metric != "cosine" && embedding->metric != "dot" &&
         embedding->metric != "euclidean") ||
        (embedding->normalization != "none" && embedding->normalization != "l2") ||
        !bounded(embedding->vector_ref, 1, 512))
        throw std::invalid_argument("embedding violates v1 metadata bounds");
}

void canonicalize_neighbors(std::vector<CapabilityNeighbor>& neighbors) {
    std::sort(neighbors.begin(), neighbors.end(), [](const auto& left, const auto& right) {
        return std::tie(left.direction, left.relation, left.entity_key, left.distance,
                        left.digest) < std::tie(right.direction, right.relation, right.entity_key,
                                                right.distance, right.digest);
    });
    neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
}

void validate_evidence(const CapabilityEvidence& evidence) {
    static const std::set<std::string> kinds = {"file",     "symbol",     "route",    "handler",
                                                "contract", "event",      "schema",   "dto",
                                                "test",     "dependency", "call-edge"};
    if (!kinds.contains(evidence.kind) || !bounded(evidence.entity_key, 1, 4096) ||
        !bounded(evidence.digest, 16, 128) || evidence.source_sequence == 0 ||
        !bounded(evidence.extractor_version, 1, 128) ||
        (evidence.path && evidence.path->size() > 4096) ||
        (evidence.symbol && evidence.symbol->size() > 2048) ||
        (evidence.line_start && *evidence.line_start == 0) ||
        (evidence.line_end && *evidence.line_end == 0) ||
        (evidence.line_start && evidence.line_end && *evidence.line_end < *evidence.line_start))
        throw std::invalid_argument("capability evidence violates v1 metadata bounds");
}

std::string summary(const CapabilitySignature& signature) {
    return signature.normalized_name + " [" + signature.technologies.front() +
           "] symbols=" + std::to_string(signature.public_symbols.size()) +
           " routes=" + std::to_string(signature.routes.size()) +
           " contracts=" + std::to_string(signature.contracts.size());
}

} // namespace

std::vector<CapabilitySignature>
CapabilitySignatureExtractor::extract(const CapabilityExtractionRequest& request) const {
    if (!uuid(request.stream.repository_id) || !uuid(request.stream.index_stream_id) ||
        request.source_sequence == 0 || !bounded(request.index_epoch, 16, 128) ||
        !bounded(request.manifest_hash, 16, 128) || !timestamp(request.extracted_at) ||
        !bounded(request.configuration_digest, 16, 128) ||
        (request.bounded_context && request.bounded_context->size() > 256))
        throw std::invalid_argument("capability extraction requires complete provenance");

    std::set<std::string> affected(request.affected_entity_keys.begin(),
                                   request.affected_entity_keys.end());
    std::vector<CapabilitySignature> signatures;
    for (const auto& input : request.entities) {
        if (request.incremental && !affected.contains(input.entity_key)) continue;
        if (!bounded(input.entity_key, 1, 4096) || input.name.empty() || input.language.empty() ||
            !bounded(input.name, 1, 512) || !bounded(input.language, 1, 256) ||
            !bounded(input.content_digest, 16, 128) || input.evidence.empty() ||
            (input.path && input.path->size() > 4096) ||
            (input.symbol && input.symbol->size() > 2048) ||
            (input.module && input.module->size() > 1024) ||
            (input.name_space && input.name_space->size() > 1024))
            throw std::invalid_argument("capability input requires metadata digest and evidence");
        // Enforce input fan-out before canonicalization. Silently collapsing an oversized input
        // would hide a malformed producer and undermine the bounded central projection contract.
        validate_strings(input.public_symbols, 2000, 1024, "public_symbols");
        validate_strings(input.routes, 1000, 2048, "routes");
        validate_strings(input.handlers, 2000, 2048, "handlers");
        validate_strings(input.contracts, 2000, 2048, "contracts");
        validate_strings(input.events, 2000, 1024, "events");
        validate_strings(input.schemas, 2000, 2048, "schemas");
        validate_strings(input.dtos, 2000, 2048, "dtos");
        validate_strings(input.internal_dependencies, 5000, 1024, "internal_dependencies");
        validate_strings(input.external_dependencies, 5000, 1024, "external_dependencies");
        validate_strings(input.tests, 2000, 4096, "tests");
        if (input.ast_digest && !bounded(*input.ast_digest, 16, 128))
            throw std::invalid_argument("AST fingerprint violates v1 metadata bounds");
        validate_neighbors(input.call_graph_neighborhood);
        validate_embedding(input.embedding);

        CapabilitySignature signature;
        signature.stream = request.stream;
        signature.source_sequence = request.source_sequence;
        signature.index_epoch = request.index_epoch;
        signature.manifest_hash = request.manifest_hash;
        signature.extracted_at = request.extracted_at;
        signature.bounded_context = request.bounded_context;
        signature.module = input.module;
        signature.name_space = input.name_space;
        signature.path = input.path;
        signature.normalized_name = normalize_capability_name(input.name);
        if (!bounded(signature.normalized_name, 1, 512))
            throw std::invalid_argument(
                "capability name normalization violates v1 metadata bounds");
        signature.public_symbols = input.public_symbols;
        signature.routes = input.routes;
        signature.handlers = input.handlers;
        signature.contracts = input.contracts;
        signature.events = input.events;
        signature.schemas = input.schemas;
        signature.dtos = input.dtos;
        signature.internal_dependencies = input.internal_dependencies;
        signature.external_dependencies = input.external_dependencies;
        signature.tests = input.tests;
        signature.call_graph_neighborhood = input.call_graph_neighborhood;
        signature.embedding = input.embedding;
        signature.technologies = {input.language};
        signature.ast_fingerprints = input.ast_digest ? std::vector<std::string>{*input.ast_digest}
                                                      : std::vector<std::string>{};
        signature.evidence = input.evidence;
        signature.evidence.push_back(CapabilityEvidence{"file",
                                                        input.entity_key,
                                                        input.path,
                                                        input.symbol,
                                                        input.content_digest,
                                                        request.source_sequence,
                                                        "v1",
                                                        {},
                                                        {}});
        signature.provenance.configuration_digest = request.configuration_digest;
        canonicalize(signature.public_symbols);
        canonicalize(signature.routes);
        canonicalize(signature.handlers);
        canonicalize(signature.contracts);
        canonicalize(signature.events);
        canonicalize(signature.schemas);
        canonicalize(signature.dtos);
        canonicalize(signature.internal_dependencies);
        canonicalize(signature.external_dependencies);
        canonicalize(signature.tests);
        canonicalize(signature.technologies);
        canonicalize(signature.ast_fingerprints);
        canonicalize_neighbors(signature.call_graph_neighborhood);
        validate_strings(signature.public_symbols, 2000, 1024, "public_symbols");
        validate_strings(signature.routes, 1000, 2048, "routes");
        validate_strings(signature.handlers, 2000, 2048, "handlers");
        validate_strings(signature.contracts, 2000, 2048, "contracts");
        validate_strings(signature.events, 2000, 1024, "events");
        validate_strings(signature.schemas, 2000, 2048, "schemas");
        validate_strings(signature.dtos, 2000, 2048, "dtos");
        validate_strings(signature.internal_dependencies, 5000, 1024, "internal_dependencies");
        validate_strings(signature.external_dependencies, 5000, 1024, "external_dependencies");
        validate_strings(signature.tests, 2000, 4096, "tests");
        validate_strings(signature.technologies, 256, 256, "technologies");
        if (signature.ast_fingerprints.size() > 2000 || signature.evidence.size() > 10000)
            throw std::invalid_argument("capability signature exceeds v1 metadata limits");
        for (const auto& fingerprint : signature.ast_fingerprints) {
            if (!bounded(fingerprint, 16, 128))
                throw std::invalid_argument("AST fingerprint violates v1 metadata bounds");
        }
        for (const auto& evidence : signature.evidence)
            validate_evidence(evidence);
        std::sort(signature.evidence.begin(), signature.evidence.end(),
                  [](const auto& left, const auto& right) {
                      return std::tie(left.kind, left.entity_key, left.path, left.symbol,
                                      left.digest, left.source_sequence, left.extractor_version,
                                      left.line_start, left.line_end) <
                             std::tie(right.kind, right.entity_key, right.path, right.symbol,
                                      right.digest, right.source_sequence, right.extractor_version,
                                      right.line_start, right.line_end);
                  });
        signature.deterministic_summary = summary(signature);
        signature.signature_id = capability_fingerprint(signature);
        signatures.push_back(std::move(signature));
    }
    std::sort(signatures.begin(), signatures.end(), [](const auto& left, const auto& right) {
        return left.signature_id < right.signature_id;
    });
    return signatures;
}

} // namespace axon::portfolio
