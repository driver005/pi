export module pi.types.sse_event;

import std;

/** One Server-Sent Event. `event` defaults to "message" when no event field was sent. */
export struct SseEvent {
    std::string event = "message";
    std::string data;
    std::string id;
    std::optional<int> retryMs;
};
