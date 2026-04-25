#pragma once
#include "server.hpp"
#include <string>

namespace axon::mcp {

struct HttpConfig {
    std::string host = "127.0.0.1";
    int         port = 7070;
    std::string group;        // if set, serve aggregate of this group
    bool        all_repos = false; // if true, serve all registered repos
};

// Runs a simple HTTP/1.1 server exposing:
//   GET  /api/graph    → { nodes, edges, meta }
//   GET  /api/overview → { top_files, top_symbols }
//   GET  /api/search?q → { files, symbols }
//   GET  /api/symbol/:name → symbol detail + callers
//   POST /api/detect-changes { ref } → detect_changes result
// Blocks until the server is stopped (Ctrl+C / SIGINT).
void run_http(ServerContext& ctx, const HttpConfig& cfg);

} // namespace axon::mcp
