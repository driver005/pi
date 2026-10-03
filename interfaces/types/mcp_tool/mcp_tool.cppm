export module pi.types.mcp_tool;

import std;
export import pi.types.json;

/** A tool a server lists in tools/list. */
export struct McpTool {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    /** JSON Schema of the arguments. */
    Json inputSchema = Json::object();
    /** Null when the server declares none. */
    Json outputSchema;
    /** Null when the server declares none (title and the read-only, destructive, ... hints). */
    Json annotations;
};
