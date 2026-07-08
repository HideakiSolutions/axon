#pragma once
#include "server.hpp"
#include <optional>
#include <string>

namespace axon::mcp {

// Starts a localhost-only HTTP listener on an ephemeral port in a background
// thread. Serves POST /rpc/tool (Bearer-token gated) by funnelling into
// call_tool(), and GET /rpc/health. Fills ctx.peer_port and ctx.peer_token.
// Returns false if the listener could not be started (proxying is then
// unavailable but the serve works exactly as before).
bool start_peer_listener(ServerContext& ctx);

// Stops the listener thread started by start_peer_listener (idempotent).
void stop_peer_listener();

// If another live axon process owns this repo's DB (per the registry),
// forward the tool call to it and return its result. Returns nullopt when
// there is no reachable owner; `error` then carries a short diagnostic.
std::optional<nlohmann::json> proxy_tool_call(ServerContext& ctx, const std::string& name,
                                              const nlohmann::json& args, std::string& error);

// True if `pid` refers to a live process on this machine.
bool pid_alive(long long pid);

// Publish this process as the repo's DB owner in the registry (idempotent).
// `port` is the endpoint answering /rpc/tool: the peer listener port for
// stdio serves, the main HTTP port for web serves.
void register_self_as_owner(ServerContext& ctx, int port);

// Remove this process's owner registration (no-op if another pid owns it).
void unregister_self_as_owner(ServerContext& ctx);

} // namespace axon::mcp
