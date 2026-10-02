#pragma once

#include <string>

enum class LogLevel { Debug, Info, Warn, Error };

/** Diagnostic sink. Implementations must be thread-safe. */
class ILogger {
public:
    virtual ~ILogger() = default;

    virtual void log(LogLevel level, const std::string& message) = 0;
};
