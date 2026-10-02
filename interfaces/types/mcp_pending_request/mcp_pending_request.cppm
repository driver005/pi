module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.types.mcp_pending_request;

import std;
export import pi.support.abort_signal;
export import pi.types.json;
export import pi.types.mcp_progress;
export import pi.types.result;

/** Bookkeeping of one request waiting for its response; guarded by the client's mutex. */
export struct McpPendingRequest {
    std::int64_t id = 0;
    std::string method;
    /** Set when the request finished: the result, or why it failed. */
    std::optional<Result<Json>> outcome;
    std::int64_t timeoutMs = 0;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    /** The spec forbids cancelling initialize. */
    bool cancellable = true;
    bool aborted = false;
    std::function<void(const McpProgress&)> onProgress;
};
