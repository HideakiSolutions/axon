#include "capability_signature.hpp"

#include <blake3.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace axon::portfolio {
namespace {

std::string digest(const std::string& input) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, input.data(), input.size());
    std::uint8_t bytes[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&hasher, bytes, sizeof(bytes));
    std::ostringstream out;
    for (auto byte : bytes)
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return out.str();
}

void append(std::ostringstream& out, const std::string& value) {
    out << value.size() << ':' << value << '|';
}

void append_optional(std::ostringstream& out, const char* field,
                     const std::optional<std::string>& value) {
    append(out, field);
    append(out, value ? "present" : "absent");
    if (value)
        append(out, *value);
}

void append_all(std::ostringstream& out, const char* field, const std::vector<std::string>& values) {
    append(out, field);
    append(out, std::to_string(values.size()));
    for (const auto& value : values)
        append(out, value);
}

} // namespace

std::string normalize_capability_name(const std::string& name) {
    std::string output;
    bool previous_space = true;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const auto current = static_cast<unsigned char>(name[index]);
        const bool word = std::isalnum(current) != 0;
        const bool camel_boundary = index > 0 && std::isupper(current) &&
            std::islower(static_cast<unsigned char>(name[index - 1]));
        if (!word) {
            if (!previous_space) {
                output += ' ';
                previous_space = true;
            }
            continue;
        }
        if (camel_boundary && !previous_space)
            output += ' ';
        output += static_cast<char>(std::tolower(current));
        previous_space = false;
    }
    if (!output.empty() && output.back() == ' ')
        output.pop_back();
    return output;
}

std::string capability_fingerprint(const CapabilitySignature& signature) {
    std::ostringstream canonical;
    append(canonical, signature.schema_version);
    append(canonical, signature.stream.repository_id);
    append(canonical, signature.stream.index_stream_id);
    append(canonical, std::to_string(signature.source_sequence));
    append(canonical, signature.index_epoch);
    append(canonical, signature.manifest_hash);
    append_optional(canonical, "bounded_context", signature.bounded_context);
    append_optional(canonical, "module", signature.module);
    append_optional(canonical, "namespace", signature.name_space);
    append_optional(canonical, "path", signature.path);
    append(canonical, signature.normalized_name);
    append(canonical, signature.deterministic_summary);
    append_all(canonical, "public_symbols", signature.public_symbols);
    append_all(canonical, "routes", signature.routes);
    append_all(canonical, "handlers", signature.handlers);
    append_all(canonical, "contracts", signature.contracts);
    append_all(canonical, "events", signature.events);
    append_all(canonical, "schemas", signature.schemas);
    append_all(canonical, "dtos", signature.dtos);
    append_all(canonical, "internal_dependencies", signature.internal_dependencies);
    append_all(canonical, "external_dependencies", signature.external_dependencies);
    append_all(canonical, "tests", signature.tests);
    append_all(canonical, "technologies", signature.technologies);
    append_all(canonical, "ast_fingerprints", signature.ast_fingerprints);
    append(canonical, "call_graph_neighborhood");
    append(canonical, std::to_string(signature.call_graph_neighborhood.size()));
    for (const auto& neighbor : signature.call_graph_neighborhood) {
        append(canonical, neighbor.direction);
        append(canonical, neighbor.relation);
        append(canonical, neighbor.entity_key);
        append(canonical, std::to_string(neighbor.distance));
        append_optional(canonical, "neighbor.digest", neighbor.digest);
    }
    append(canonical, "embedding");
    append(canonical, signature.embedding ? "present" : "absent");
    if (signature.embedding) {
        append(canonical, signature.embedding->model_id);
        append(canonical, std::to_string(signature.embedding->dimension));
        append(canonical, signature.embedding->metric);
        append(canonical, signature.embedding->normalization);
        append(canonical, signature.embedding->vector_ref);
    }
    append(canonical, "evidence");
    append(canonical, std::to_string(signature.evidence.size()));
    for (const auto& evidence : signature.evidence) {
        append(canonical, evidence.kind);
        append(canonical, evidence.entity_key);
        append_optional(canonical, "evidence.path", evidence.path);
        append_optional(canonical, "evidence.symbol", evidence.symbol);
        append(canonical, evidence.digest);
        append(canonical, std::to_string(evidence.source_sequence));
        append(canonical, evidence.extractor_version);
        append(canonical, evidence.line_start ? std::to_string(*evidence.line_start) : "");
        append(canonical, evidence.line_end ? std::to_string(*evidence.line_end) : "");
    }
    append(canonical, signature.provenance.extractor_id);
    append(canonical, signature.provenance.extractor_version);
    append(canonical, signature.provenance.source_schema_version);
    append(canonical, signature.provenance.configuration_digest);
    return digest(canonical.str());
}

} // namespace axon::portfolio
