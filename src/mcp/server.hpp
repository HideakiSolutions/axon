#pragma once
#include "../core/config.hpp"
#include "../core/db.hpp"
#include "../core/graph.hpp"
#include "../core/embeddings.hpp"
#include <memory>
#include <string>

namespace axon::mcp {

struct ServerContext {
    Config                        cfg;
    std::filesystem::path         binary_dir;
    std::unique_ptr<Database>     db;
    std::unique_ptr<EmbeddingModel> model;
    DependencyGraph               graph;
    std::string                   db_error;

    bool db_ready()    const { return db    != nullptr; }
    bool model_ready() const { return model != nullptr; }
};

// Runs the MCP stdio server loop (blocks until stdin closes)
void run_stdio(ServerContext& ctx);

} // namespace axon::mcp
