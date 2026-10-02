export module pi.mcp.i_mcp_server_manager;

import std;
export import pi.types.mcp_server_config;
export import pi.types.mcp_server_status;

/**
 * The MCP servers of one session: connects them and keeps their tools registered with the tool
 * registry (tools of servers that connect late, or whose tool list changes, appear and disappear
 * on their own).
 */
export class IMcpServerManager {
public:
    virtual ~IMcpServerManager() = default;

    /**
     * Connects every enabled server with visible tools, in parallel, and returns when all have
     * settled or `startupWait` elapsed. Slow servers keep connecting in the background.
     */
    virtual void start(const std::vector<McpServerConfig>& servers, const std::string& cwd,
                       std::chrono::milliseconds startupWait) = 0;
    virtual std::vector<McpServerStatus> status() const = 0;
    /** Disconnects every server and unregisters its tools. Idempotent. */
    virtual void close() = 0;
};
