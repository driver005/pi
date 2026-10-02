export module pi.types.mcp_server_status;

import std;
export import pi.types.mcp_server_state;

/** Snapshot of one server for diagnostics. */
export struct McpServerStatus {
    std::string name;
    McpServerState state = McpServerState::Connecting;
    /** Why the server failed or disconnected; empty otherwise. */
    std::string error;
    std::size_t toolCount = 0;
};
