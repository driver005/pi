export module pi.types.mcp_stream_outcome;

import std;
export import pi.types.error;

/** How one HTTP exchange that carries a message stream ended. */
export struct McpStreamOutcome {
    /** The server answered 405: it offers no stream here. */
    bool unsupported = false;
    /** Whether the failure is worth reconnecting for (network trouble, 408, 429, 5xx). */
    bool retryable = false;
    /** Empty when the stream ended cleanly. */
    std::optional<Error> error;
};
