export module pi.types.mcp_config_load_options;

import std;

export struct McpConfigLoadOptions {
    std::string agentDir;
    std::string cwd;
    /** Project <cwd>/.pi/mcp.json is read only when the project is trusted. */
    bool projectTrusted = false;
};
