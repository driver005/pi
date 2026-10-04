export module pi.types.crash_record;

import std;

/** A crash pi recorded in `crashes.json` so the next start (or a bug report) can mention it. */
export struct CrashRecord {
    std::string timestamp;
    std::string version;
    /** "uncaught_exception" or "fatal_error" (a fatal signal or std::terminate here). */
    std::string kind;
    std::string message;
    std::optional<std::string> stack;
    std::optional<std::string> sessionFile;
    std::string cwd;
    bool notified = false;
};
