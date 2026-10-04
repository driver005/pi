export module pi.mcp.i_mcp_server_registrar;

import std;
export import pi.types.json;
export import pi.types.result;

/** Where plugins register MCP servers for the session: connected next to the configured servers and withdrawn with the plugin. */
export class IMcpServerRegistrar {
public:
    virtual ~IMcpServerRegistrar() = default;

    /**
     * Registers the server `name` for `owner` (the plugin), with the config of an `mcpServers` entry of mcp.json. Registering
     * a name again replaces the owner's earlier registration. Fails for an invalid config and for a name another owner
     * registered. A server of the same name in mcp.json takes precedence and the registration is ignored.
     */
    virtual Result<void> registerServer(const std::string& owner, const std::string& name, const Json& config) = 0;
    /** Removes the owner's server and closes its connection; other owners' servers are untouched. */
    virtual void unregisterServer(const std::string& owner, const std::string& name) = 0;
};
