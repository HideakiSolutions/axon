#pragma once
#include "../core/config.hpp"
#include "../core/db.hpp"
#include "../core/graph.hpp"
#include "../core/embeddings.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <chrono>

namespace axon::mcp {

struct ServerContext {
    Config cfg;
    std::filesystem::path binary_dir;
    std::unique_ptr<Database> db;
    std::unique_ptr<EmbeddingModel> model;
    DependencyGraph graph;
    std::string db_error;

    // Peer-proxy state. tool_mutex serializes tool execution between the
    // stdio loop and the peer listener thread (unique_ptr keeps the struct
    // movable). peer_port/peer_token identify this process's own listener;
    // owner_registered tracks whether we've published ourselves as the DB
    // owner in the registry.
    std::unique_ptr<std::mutex> tool_mutex = std::make_unique<std::mutex>();
    int peer_port = 0;
    std::string peer_token;
    bool owner_registered = false;
    std::chrono::steady_clock::time_point last_tool_activity = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_owner_heartbeat = std::chrono::steady_clock::now();

    bool db_ready() const { return db != nullptr; }
    bool model_ready() const { return model != nullptr; }
};

// Executes one MCP tool call under ctx.tool_mutex. Safe to call from both
// the stdio loop and the peer listener thread.
nlohmann::json call_tool(const std::string& name, const nlohmann::json& args, ServerContext& ctx);

// Runs the MCP stdio server loop (blocks until stdin closes)
void run_stdio(ServerContext& ctx);

} // namespace axon::mcp
