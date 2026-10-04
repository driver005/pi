export module pi.platform.i_process_runner;

import std;
export import pi.types.process_request;
export import pi.types.process_result;
export import pi.types.result;

/** Runs a child process in its own process group and waits for it. */
export class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;

    /**
     * Blocks until the child exits, times out or the request's signal aborts; on the latter
     * two the whole process group is terminated (SIGTERM, then SIGKILL after a grace period).
     * Error means the process could not be started at all.
     */
    virtual Result<ProcessResult> run(const ProcessRequest& request) = 0;
};
