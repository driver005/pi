#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "interfaces/types/sse_event/sse_event.h"

/** Incremental text/event-stream parser; feed arbitrary byte chunks, receive whole events. */
class SseParser {
public:
    /** Appends bytes and returns every event completed by them. */
    std::vector<SseEvent> feed(std::string_view chunk);

    /** Flushes a trailing event that was not terminated by a blank line. */
    std::vector<SseEvent> finish();

private:
    void handleLine(const std::string& line, std::vector<SseEvent>& out);
    void dispatch(std::vector<SseEvent>& out);

    std::string m_buffer;
    std::string m_eventName;
    std::string m_data;
    std::string m_lastId;
    std::optional<int> m_retry;
    bool m_hasData = false;
    bool m_skipLf = false;
};
