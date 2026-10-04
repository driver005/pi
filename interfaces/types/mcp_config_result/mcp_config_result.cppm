export module pi.types.mcp_config_result;

import std;
export import pi.types.mcp_server_config;

/** Servers from the global and project mcp.json files plus the problems found while reading them. */
export struct McpConfigResult {
    /** Disabled servers are included with enabled = false. */
    std::vector<McpServerConfig> servers;
    std::vector<std::string> errors;
    /** The project mcp.json when the project is trusted. */
    std::optional<std::string> projectConfig;
};
