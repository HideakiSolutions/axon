#include "falkordb_capability_graph.hpp"

#include <hiredis/hiredis.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace axon::portfolio {
namespace {

bool safe_identifier(const std::string& value) {
    return !value.empty() && value.size() <= 80 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isalnum(character) || character == '_' || character == '-';
           });
}

bool safe_text(const std::string& value, const std::size_t maximum) {
    return !value.empty() && value.size() <= maximum && value.find('\0') == std::string::npos;
}

std::string quote(const std::string& value) {
    std::string result = "'";
    for (const char character : value) {
        if (character == '\\' || character == '\'') {
            result += '\\';
        }
        result += character;
    }
    return result + "'";
}

void collect_strings(redisReply* reply, std::vector<std::string>& output) {
    if (reply == nullptr) {
        return;
    }
    if ((reply->type == REDIS_REPLY_STRING || reply->type == REDIS_REPLY_STATUS) && reply->str != nullptr) {
        output.emplace_back(reply->str, reply->len);
    }
    for (std::size_t index = 0; index < reply->elements; ++index) {
        collect_strings(reply->element[index], output);
    }
}

std::string logical_id(const RepositoryStreamKey& stream, const std::string& signature_id) {
    return graph_capability_id(stream, signature_id);
}

std::string physical_id(const RepositoryStreamKey& stream,
                        const std::string& signature_id,
                        const std::string& generation) {
    return logical_id(stream, signature_id) + ":" + generation;
}

void validate_neighbor(const CapabilityNeighbor& neighbor) {
    if ((neighbor.direction != "incoming" && neighbor.direction != "outgoing") ||
        !safe_text(neighbor.relation, 256) || !safe_text(neighbor.entity_key, 4096) ||
        neighbor.distance == 0 || neighbor.distance > 8 ||
        (neighbor.digest && (neighbor.digest->size() < 16 || !safe_text(*neighbor.digest, 128)))) {
        throw std::invalid_argument("invalid graph neighbor");
    }
}

std::string relationship_properties(const CapabilityNeighbor& neighbor, const std::string& generation) {
    std::string result = "relation:" + quote(neighbor.relation) + ",generation:" + quote(generation) +
                         ",direction:" + quote(neighbor.direction) + ",distance:" +
                         std::to_string(neighbor.distance);
    if (neighbor.digest) {
        result += ",digest:" + quote(*neighbor.digest);
    }
    return result;
}

} // namespace

FalkorDbCapabilityGraph::FalkorDbCapabilityGraph(std::string host, const int port, std::string graph_name)
    : host_(std::move(host)), graph_name_(std::move(graph_name)), port_(port) {
    if (host_.empty() || port_ <= 0 || port_ > 65535 || !safe_identifier(graph_name_)) {
        throw std::invalid_argument("invalid FalkorDB configuration");
    }
    context_ = redisConnectWithTimeout(host_.c_str(), port_, timeval{3, 0});
    if (context_ == nullptr || context_->err != 0) {
        if (context_ != nullptr) {
            redisFree(context_);
            context_ = nullptr;
        }
        throw std::runtime_error("FalkorDB unavailable");
    }
}

FalkorDbCapabilityGraph::~FalkorDbCapabilityGraph() {
    if (context_ != nullptr) {
        redisFree(context_);
    }
}

void FalkorDbCapabilityGraph::execute(const std::string& query) const {
    std::lock_guard lock(mutex_);
    auto* reply = static_cast<redisReply*>(
        redisCommand(context_, "GRAPH.QUERY %s %s --compact", graph_name_.c_str(), query.c_str()));
    if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
        const auto detail = reply != nullptr && reply->str != nullptr ? std::string(reply->str, reply->len) : "no reply";
        if (reply != nullptr) {
            freeReplyObject(reply);
        }
        throw std::runtime_error("FalkorDB graph query failed: " + detail);
    }
    freeReplyObject(reply);
}

void FalkorDbCapabilityGraph::replace_repository(const RepositoryStreamKey& stream,
                                                 const std::string& generation,
                                                 const std::vector<CapabilitySignature>& signatures) {
    if (!safe_identifier(generation) || !safe_identifier(stream.repository_id) ||
        !safe_identifier(stream.index_stream_id) || signatures.size() > 10000) {
        throw std::invalid_argument("invalid graph projection input");
    }

    for (const auto& signature : signatures) {
        if (signature.stream != stream || !safe_identifier(signature.signature_id) ||
            !safe_identifier(signature.index_epoch)) {
            throw std::invalid_argument("invalid graph signature");
        }
        for (const auto& neighbor : signature.call_graph_neighborhood) {
            validate_neighbor(neighbor);
        }
    }

    // Write a complete generation before publishing it through AxonGraphState. Readers only use
    // the published generation, so a failed replacement cannot expose a partial repository graph.
    for (const auto& signature : signatures) {
        const auto source_logical_id = logical_id(stream, signature.signature_id);
        const auto source_physical_id = physical_id(stream, signature.signature_id, generation);
        execute("MERGE (n:AxonCapability {id:" + quote(source_physical_id) + "}) SET n.logical_id=" +
                quote(source_logical_id) + ",n.repository_id=" + quote(stream.repository_id) +
                ",n.index_stream_id=" + quote(stream.index_stream_id) + ",n.signature_id=" +
                quote(signature.signature_id) + ",n.generation=" + quote(generation) + ",n.epoch=" +
                quote(signature.index_epoch));

        for (const auto& neighbor : signature.call_graph_neighborhood) {
            const auto target_logical_id = logical_id(stream, neighbor.entity_key);
            const auto target_physical_id = physical_id(stream, neighbor.entity_key, generation);
            execute("MERGE (b:AxonCapability {id:" + quote(target_physical_id) + "}) SET b.logical_id=" +
                    quote(target_logical_id) + ",b.repository_id=" + quote(stream.repository_id) +
                    ",b.index_stream_id=" + quote(stream.index_stream_id) + ",b.signature_id=" +
                    quote(neighbor.entity_key) + ",b.generation=" + quote(generation));

            const auto& edge_source = neighbor.direction == "outgoing" ? source_physical_id : target_physical_id;
            const auto& edge_target = neighbor.direction == "outgoing" ? target_physical_id : source_physical_id;
            execute("MATCH (a:AxonCapability {id:" + quote(edge_source) + "}), (b:AxonCapability {id:" +
                    quote(edge_target) + "}) MERGE (a)-[:RELATES_TO {" +
                    relationship_properties(neighbor, generation) + "}]->(b)");
        }
    }

    const auto state_id = stream.repository_id + ":" + stream.index_stream_id;
    execute("MERGE (s:AxonGraphState {id:" + quote(state_id) + "}) SET s.repository_id=" +
            quote(stream.repository_id) + ",s.index_stream_id=" + quote(stream.index_stream_id) +
            ",s.generation=" + quote(generation));
    execute("MATCH (n:AxonCapability {repository_id:" + quote(stream.repository_id) + ",index_stream_id:" +
            quote(stream.index_stream_id) + "}) WHERE n.generation <> " + quote(generation) + " DETACH DELETE n");
}

GraphTraversal FalkorDbCapabilityGraph::traverse(const RepositoryStreamKey& stream,
                                                 const std::string& signature_id,
                                                 const std::size_t max_depth,
                                                 const std::size_t max_nodes) const {
    if (!safe_identifier(stream.repository_id) || !safe_identifier(stream.index_stream_id) ||
        !safe_identifier(signature_id) || max_depth == 0 || max_depth > 3 || max_nodes == 0 || max_nodes > 100) {
        throw std::invalid_argument("invalid graph traversal bounds");
    }

    const auto query =
        "MATCH (s:AxonGraphState {repository_id:" + quote(stream.repository_id) + ",index_stream_id:" +
        quote(stream.index_stream_id) + "}), (n:AxonCapability {repository_id:" + quote(stream.repository_id) +
        ",index_stream_id:" + quote(stream.index_stream_id) + ",signature_id:" + quote(signature_id) +
        "}) WHERE n.generation=s.generation MATCH (n)-[*0.." + std::to_string(max_depth) +
        "]->(m:AxonCapability) WHERE m.generation=s.generation AND m.repository_id=s.repository_id "
        "AND m.index_stream_id=s.index_stream_id "
        "RETURN DISTINCT m.logical_id ORDER BY m.logical_id LIMIT " + std::to_string(max_nodes + 1);

    std::lock_guard lock(mutex_);
    auto* reply = static_cast<redisReply*>(
        redisCommand(context_, "GRAPH.QUERY %s %s --compact", graph_name_.c_str(), query.c_str()));
    if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
        const auto detail = reply != nullptr && reply->str != nullptr ? std::string(reply->str, reply->len) : "no reply";
        if (reply != nullptr) {
            freeReplyObject(reply);
        }
        throw std::runtime_error("FalkorDB graph query failed: " + detail);
    }

    GraphTraversal result;
    std::vector<std::string> values;
    collect_strings(reply, values);
    freeReplyObject(reply);
    const auto prefix = stream.repository_id + ":" + stream.index_stream_id + ":";
    for (const auto& value : values) {
        if (value.starts_with(prefix) &&
            std::find(result.capability_ids.begin(), result.capability_ids.end(), value) == result.capability_ids.end()) {
            result.capability_ids.push_back(value);
        }
    }
    if (result.capability_ids.size() > max_nodes) {
        result.capability_ids.resize(max_nodes);
        result.truncated = true;
    }
    return result;
}

} // namespace axon::portfolio
