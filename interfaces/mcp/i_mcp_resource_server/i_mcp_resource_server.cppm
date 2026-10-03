export module pi.mcp.i_mcp_resource_server;

import std;
export import pi.types.json;
export import pi.types.mcp_page;
export import pi.types.mcp_request_options;
export import pi.types.result;

/** The resources of one MCP server, reached on demand (connecting or reconnecting when needed). Thread-safe. */
export class IMcpResourceServer {
public:
    virtual ~IMcpResourceServer() = default;

    virtual std::string serverName() const = 0;
    /** Per-request timeout of this server. */
    virtual std::int64_t requestTimeoutMs() const = 0;

    virtual Result<McpPage> resourcesPage(const std::optional<std::string>& cursor, const McpRequestOptions& options) = 0;
    /** Servers without templates give an empty page. */
    virtual Result<McpPage> resourceTemplatesPage(const std::optional<std::string>& cursor, const McpRequestOptions& options) = 0;
    virtual Result<std::vector<Json>> allResources(const McpRequestOptions& options) = 0;
    virtual Result<std::vector<Json>> allResourceTemplates(const McpRequestOptions& options) = 0;
    /** `{contents: [{uri, mimeType?, text | blob}]}`. */
    virtual Result<Json> readResource(const std::string& uri, const McpRequestOptions& options) = 0;
};
