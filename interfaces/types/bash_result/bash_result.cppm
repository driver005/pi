export module pi.types.bash_result;

import std;

/** Outcome of a user-run shell command. */
export struct BashResult {
    /** Sanitized stdout+stderr, truncated to the tail when too long. */
    std::string output;
    /** Absent when the command was cancelled or killed by a signal. */
    std::optional<int> exitCode;
    bool cancelled = false;
    bool truncated = false;
    /** File holding the complete output when it exceeded the limits. */
    std::optional<std::string> fullOutputPath;
};
