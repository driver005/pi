export module pi.platform.i_child_process;

import std;
export import pi.types.result;

/**
 * A running child process with pipes to its stdin, stdout and stderr, in its own process group.
 * Reads block; use one thread per stream. Destroying the object terminates the group.
 */
export class IChildProcess {
public:
    virtual ~IChildProcess() = default;

    /** Writes to the child's stdin. Error once the child closed it or exited. */
    virtual Result<void> write(std::string_view bytes) = 0;
    virtual void closeStdin() = 0;

    /** Blocks for the next chunk of stdout; nullopt at end of stream. */
    virtual std::optional<std::string> readOutput() = 0;
    /** Blocks for the next chunk of stderr; nullopt at end of stream. */
    virtual std::optional<std::string> readError() = 0;

    /** Waits up to `timeout` for the child to exit; its exit code (-1 when killed by a signal). */
    virtual std::optional<int> waitForExit(std::chrono::milliseconds timeout) = 0;

    /** SIGTERM to the whole process group. */
    virtual void terminate() = 0;
    /** SIGKILL to the whole process group. */
    virtual void kill() = 0;
};
