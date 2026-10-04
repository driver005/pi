export module pi.types.mcp_call_result;

import std;
export import pi.types.json;

/** The result of tools/call. Content blocks stay JSON, as the server sent them. */
export struct McpCallResult {
    Json content = Json::array();
    /** Null when absent. */
    Json structuredContent;
    bool isError = false;
};
