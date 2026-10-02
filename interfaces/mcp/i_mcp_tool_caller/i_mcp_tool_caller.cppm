module;

#include <nlohmann/json.hpp>

export module pi.mcp.i_mcp_tool_caller;

import std;
export import pi.types.json;
export import pi.types.mcp_call_result;
export import pi.types.mcp_request_options;
export import pi.types.result;

/** Calls tools of one MCP server, connecting (or reconnecting) when needed. Thread-safe. */
export class IMcpToolCaller {
public:
    virtual ~IMcpToolCaller() = default;

    virtual Result<McpCallResult> callTool(const std::string& name, const Json& arguments,
                                           const McpRequestOptions& options) = 0;
};
