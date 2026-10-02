#include "interfaces/support/sse_parser/sse_parser.h"

#include <charconv>

std::vector<SseEvent> SseParser::feed(std::string_view chunk) {
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

std::vector<SseEvent> SseParser::finish() {
    std::vector<SseEvent> out;
    if (!m_buffer.empty()) {
        handleLine(m_buffer, out);
        m_buffer.clear();
    }
    dispatch(out);
    return out;
}

void SseParser::handleLine(const std::string& line, std::vector<SseEvent>& out) {
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
        }
    }
}

void SseParser::dispatch(std::vector<SseEvent>& out) {
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
