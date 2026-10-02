#pragma once

#include <string>

/** Outcome of a finished process. exitCode is -1 when it was killed by a signal. */
struct ProcessResult {
    int exitCode = -1;
    int termSignal = 0;
    bool timedOut = false;
    bool aborted = false;
    std::string output;
};
