#pragma once

#include "db.hpp"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace axon {

// Shared, read-only JSON boundary used by CLI and MCP.
nlohmann::json run_impact_query(const std::string& name,
                                const nlohmann::json& args,
                                Database& db,
                                const std::filesystem::path& project_root);

} // namespace axon
