export module pi.types.mcp_exposure;

/**
 * How a server's tools reach the model. Codemode and tool_search are not ported, so every mode
 * except Hidden offers the tools directly; Hidden registers nothing.
 */
export enum class McpExposure { Codemode, Deferred, Direct, Hidden };
