export module pi.platform.i_logger;

import std;

export enum class LogLevel { Debug, Info, Warn, Error };

/** Diagnostic sink. Implementations must be thread-safe. */
export class ILogger {
public:
    virtual ~ILogger() = default;

    virtual void log(LogLevel level, const std::string& message) = 0;
};
