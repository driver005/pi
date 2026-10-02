#pragma once

#include <mutex>
#include <string>

#include "interfaces/platform/i_clock/i_clock.h"
#include "interfaces/platform/i_logger/i_logger.h"

/** ILogger writing "<iso time> [level] message" lines to stderr; drops lines below minLevel. */
class StderrLogger : public ILogger {
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
