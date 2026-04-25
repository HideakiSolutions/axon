#pragma once
#include "db.hpp"
#include "config.hpp"
#include <filesystem>
#include <vector>
#include <string>

namespace axon {

struct RouteInfo {
    std::string method;      // GET, POST, PUT, DELETE, ANY
    std::string path;        // /api/users/:id
    std::string handler_file;// relative path from project root
    std::string framework;   // nextjs|express|fastapi|django|flask|unknown
    int64_t     file_id = 0;
};

// Scan project for HTTP routes. Detects:
//   - Next.js: app/**/route.{ts,js}, pages/api/**
//   - Express: app.get/post/put/delete/use(...) in .js/.ts files
//   - FastAPI/Flask: @router.get/post, @app.route decorators in .py files
// Upserts results into the `routes` table.
int index_routes(const Config& cfg, Database& db);

std::vector<RouteInfo> get_all_routes(Database& db);

} // namespace axon
