#pragma once
#include "../mcp/server.hpp"

namespace axon::lsp {

// Runs a minimal Language Server Protocol loop over stdio.
void run_stdio(axon::mcp::ServerContext& ctx);

} // namespace axon::lsp
