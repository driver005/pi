export module pi.types.mcp_http_exchange;

import std;
export import pi.types.error;

/**
 * Hand-off between a request worker and the caller of send(): the worker publishes once, when
 * the response headers are known to be acceptable or the exchange failed, and send() returns then.
 */
export struct McpHttpExchange {
    std::mutex mutex;
    std::condition_variable ready;
    bool published = false;
    std::optional<Error> error;
};
