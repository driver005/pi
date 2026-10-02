export module pi.types.process_result;

import std;

/** Outcome of a finished process. exitCode is -1 when it was killed by a signal. */
export struct ProcessResult {
    int exitCode = -1;
    int termSignal = 0;
    bool timedOut = false;
    bool aborted = false;
    std::string output;
};
