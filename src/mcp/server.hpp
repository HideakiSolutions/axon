#pragma once
#include "../core/config.hpp"
#include "../core/db.hpp"
#include "../core/graph.hpp"
#include "../core/embeddings.hpp"
#include <memory>

namespace axon::mcp {

struct ServerContext {
    Config                        cfg;
    std::unique_ptr<Database>     db;
    std::unique_ptr<EmbeddingModel> model;
    DependencyGraph               graph;

    bool db_ready()    const { return db    != nullptr; }
    bool model_ready() const { return model != nullptr; }
};

// Runs the MCP stdio server loop (blocks until stdin closes)
void run_stdio(ServerContext& ctx);

} // namespace axon::mcp
