export module pi.mcp.i_mcp_connector;

import std;
export import pi.mcp.i_mcp_client;
export import pi.types.mcp_client_options;
export import pi.types.mcp_server_config;
export import pi.types.result;

/**
 * Builds the transport for a configured server (resolving `${VAR}` and `!cmd` values, relative
 * paths and credentials), connects an MCP client over it and runs the handshake. Errors keep the
 * codes of the transport and client ("auth_required", "http:<status>", "timeout", ...); for stdio
 * servers the message ends with the tail of the server's stderr.
 */
export class IMcpConnector {
public:
    virtual ~IMcpConnector() = default;

    virtual Result<std::unique_ptr<IMcpClient>> connect(const McpServerConfig& config,
                                                        const std::string& cwd,
                                                        const McpClientOptions& options) = 0;
};
