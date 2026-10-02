#pragma once

#include "interfaces/types/process_request/process_request.h"
#include "interfaces/types/process_result/process_result.h"
#include "interfaces/types/result/result.h"

/** Runs a child process in its own process group and waits for it. */
class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;

    /**
     * Blocks until the child exits, times out or the request's signal aborts; on the latter
     * two the whole process group is terminated (SIGTERM, then SIGKILL after a grace period).
     * Error means the process could not be started at all.
     */
    virtual Result<ProcessResult> run(const ProcessRequest& request) = 0;
};
