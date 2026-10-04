export module pi.types.mcp_server_state;

/**
 * Where a configured server stands. Disconnected: the connection dropped and the next call
 * reconnects. NeedsAuth: the server wants credentials this build cannot obtain by itself.
 */
export enum class McpServerState { Disabled, Connecting, Connected, Disconnected, NeedsAuth, Failed, Closed };
