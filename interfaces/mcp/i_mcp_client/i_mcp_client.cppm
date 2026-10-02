module;

#include <nlohmann/json.hpp>

export module pi.mcp.i_mcp_client;

import std;
export import pi.types.json;
export import pi.types.mcp_call_result;
export import pi.types.mcp_request_options;
export import pi.types.mcp_tool;
export import pi.types.result;

/**
 * A connected MCP client: requests are blocking calls (any thread), failures are Errors with the
 * codes "timeout", "aborted", "closed", "protocol" or "rpc:<json-rpc code>".
 */
export class IMcpClient {
public:
    virtual ~IMcpClient() = default;

    virtual Result<Json> request(const std::string& method, const Json& params, const McpRequestOptions& options) = 0;
    virtual Result<void> notify(const std::string& method, const Json& params) = 0;

    /** Every tool, following the pagination cursors. */
    virtual Result<std::vector<McpTool>> listTools(const McpRequestOptions& options) = 0;
    virtual Result<McpCallResult> callTool(const std::string& name, const Json& arguments,
                                           const McpRequestOptions& options) = 0;
    /** Every resource, following the pagination cursors. */
    virtual Result<std::vector<Json>> listResources(const McpRequestOptions& options) = 0;
    /** Every resource template; servers without templates give an empty list. */
    virtual Result<std::vector<Json>> listResourceTemplates(const McpRequestOptions& options) = 0;
    virtual Result<Json> readResource(const std::string& uri, const McpRequestOptions& options) = 0;

    /** Capabilities and instructions the server announced in initialize (null / nullopt before connect). */
    virtual Json serverCapabilities() const = 0;
    virtual std::optional<std::string> instructions() const = 0;
    virtual bool connected() const = 0;

    /** Runs on a dispatcher thread, so the listener may issue requests itself. */
    virtual void onNotification(const std::string& method, std::function<void(const Json&)> listener) = 0;
    virtual void onClose(std::function<void()> listener) = 0;
    virtual void close() = 0;
};
