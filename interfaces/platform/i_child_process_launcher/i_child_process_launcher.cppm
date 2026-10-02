export module pi.platform.i_child_process_launcher;

import std;
export import pi.platform.i_child_process;
export import pi.types.process_request;
export import pi.types.result;

/**
 * Starts long-lived child processes that are talked to over pipes (MCP stdio servers). The request's
 * stdinData, timeout, onOutput, captureOutput and signal fields are not used; stdout and stderr stay
 * separate streams.
 */
export class IChildProcessLauncher {
public:
    virtual ~IChildProcessLauncher() = default;

    virtual Result<std::unique_ptr<IChildProcess>> launch(const ProcessRequest& request) = 0;
};
