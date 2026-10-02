module;

#include <cstdio>
#include <ctime>

export module pi.base.stderr_logger;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_logger;

/** ILogger writing "<iso time> [level] message" lines to stderr; drops lines below minLevel. */
export class StderrLogger : public ILogger {
public:
    StderrLogger(const IClock& clock, LogLevel minLevel);

    void log(LogLevel level, const std::string& message) override;

private:
    std::string levelName(LogLevel level) const;
    std::string timestamp() const;

    const IClock& m_clock;
    LogLevel m_minLevel;
    std::mutex m_mutex;
};

StderrLogger::StderrLogger(const IClock& clock, LogLevel minLevel)
    : m_clock(clock), m_minLevel(minLevel) {}

std::string StderrLogger::levelName(LogLevel level) const {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

std::string StderrLogger::timestamp() const {
    const std::int64_t ms = m_clock.nowMs();
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<int>(ms % 1000));
    return buffer;
}

void StderrLogger::log(LogLevel level, const std::string& message) {
    if (level < m_minLevel) {
        return;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::fprintf(stderr, "%s [%s] %s\n", timestamp().c_str(), levelName(level).c_str(),
                 message.c_str());
}
