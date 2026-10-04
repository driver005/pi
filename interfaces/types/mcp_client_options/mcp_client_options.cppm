export module pi.types.mcp_client_options;

import std;
export import pi.types.json;

/** What an MCP client announces about itself and how long it waits by default. */
export struct McpClientOptions {
    std::string name = "pi";
    std::string version = "0";
    std::optional<std::string> title;
    std::int64_t requestTimeoutMs = 30000;
    /** Roots offered to the server through roots/list: [{"uri": ..., "name": ...}]. Null: no roots capability. */
    Json roots;
};
