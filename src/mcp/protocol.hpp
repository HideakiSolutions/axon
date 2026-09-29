#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <optional>

namespace axon::mcp {

using json = nlohmann::json;

struct Request {
    std::string jsonrpc;
    json id;
    std::string method;
    json params;
};

inline json make_response(const json& id, const json& result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

inline json make_error(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

inline json make_tool_result(const json& content, bool is_error = false) {
    return {{"content", json::array({{{"type", "text"}, {"text", content.dump(2)}}})},
            {"isError", is_error}};
}

constexpr int PARSE_ERROR = -32700;
constexpr int INVALID_REQUEST = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS = -32602;
constexpr int INTERNAL_ERROR = -32603;

} // namespace axon::mcp
