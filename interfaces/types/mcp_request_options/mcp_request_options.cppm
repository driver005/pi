export module pi.types.mcp_request_options;

import std;
export import pi.support.abort_signal;
export import pi.types.mcp_progress;

/** Per-request settings of an MCP call. */
export struct McpRequestOptions {
    std::shared_ptr<AbortSignal> signal;
    /** Zero or less disables the timeout; nullopt uses the client's default. Progress resets it. */
    std::optional<std::int64_t> timeoutMs;
    /** Called on the transport's reader thread for each progress notification. */
    std::function<void(const McpProgress&)> onProgress;
};
