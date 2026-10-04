module;

#include <cstdint>

export module pi.types.task_invocation;

import std;
export import pi.support.abort_signal;
export import pi.support.wait_gate;

/** One in-memory execution of a task in run or abort mode. */
export struct TaskInvocation {
    std::int64_t taskId = 0;
    std::int64_t conversationId = 0;
    /** "run" or "abort". */
    std::string mode = "run";
    /** Aborted when the run is signalled, the harness closes, or the invocation ends. */
    AbortSignal signal;
    std::atomic<bool> ended{false};
    /** Opens when the invocation's thread has finished. */
    WaitGate done;
};
