export module pi.support.sse_parser;

import std;
export import pi.types.sse_event;

/** Incremental text/event-stream parser; feed arbitrary byte chunks, receive whole events. */
export class SseParser {
public:
    /** Appends bytes and returns every event completed by them. */
    std::vector<SseEvent> feed(std::string_view chunk) {
        std::vector<SseEvent> out;
        for (const char c : chunk) {
            if (m_skipLf) {
                m_skipLf = false;
                if (c == '\n') {
                    continue;
                }
            }
            if (c == '\r' || c == '\n') {
                m_skipLf = c == '\r';
                handleLine(m_buffer, out);
                m_buffer.clear();
            } else {
                m_buffer.push_back(c);
            }
        }
        return out;
    }

    /** Flushes a trailing event that was not terminated by a blank line. */
    std::vector<SseEvent> finish() {
        std::vector<SseEvent> out;
        if (!m_buffer.empty()) {
            handleLine(m_buffer, out);
            m_buffer.clear();
        }
        dispatch(out);
        return out;
    }

    /** The last `id` field seen, including on events without data (resumption priming). */
    const std::string& lastEventId() const {
        return m_lastId;
    }

    /** The last valid `retry` field seen, in milliseconds. */
    std::optional<int> lastRetryMs() const {
        return m_lastRetry;
    }

private:
    void handleLine(const std::string& line, std::vector<SseEvent>& out) {
        if (line.empty()) {
            dispatch(out);
            return;
        }
        if (line[0] == ':') {
            return;
        }
        const std::size_t colon = line.find(':');
        const std::string field = line.substr(0, colon);
        std::string value = colon == std::string::npos ? "" : line.substr(colon + 1);
        if (!value.empty() && value[0] == ' ') {
            value.erase(0, 1);
        }
        if (field == "event") {
            m_eventName = value;
        } else if (field == "data") {
            if (m_hasData) {
                m_data.push_back('\n');
            }
            m_data += value;
            m_hasData = true;
        } else if (field == "id") {
            m_lastId = value;
        } else if (field == "retry") {
            int parsed = 0;
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (ec == std::errc() && ptr == value.data() + value.size()) {
                m_retry = parsed;
                m_lastRetry = parsed;
            }
        }
    }

    void dispatch(std::vector<SseEvent>& out) {
        if (m_hasData) {
            SseEvent event;
            if (!m_eventName.empty()) {
                event.event = m_eventName;
            }
            event.data = std::move(m_data);
            event.id = m_lastId;
            event.retryMs = m_retry;
            out.push_back(std::move(event));
        }
        m_eventName.clear();
        m_data.clear();
        m_hasData = false;
        m_retry.reset();
    }

    std::string m_buffer;
    std::string m_eventName;
    std::string m_data;
    std::string m_lastId;
    std::optional<int> m_retry;
    std::optional<int> m_lastRetry;
    bool m_hasData = false;
    bool m_skipLf = false;
};
