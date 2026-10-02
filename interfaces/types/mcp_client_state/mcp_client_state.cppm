export module pi.types.mcp_client_state;

/** Connection state of an MCP client. */
export enum class McpClientState { Idle, Connecting, Connected, Closed };
