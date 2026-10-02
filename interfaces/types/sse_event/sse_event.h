#pragma once

#include <optional>
#include <string>

/** One Server-Sent Event. `event` defaults to "message" when no event field was sent. */
struct SseEvent {
    std::string event = "message";
    std::string data;
    std::string id;
    std::optional<int> retryMs;
};
