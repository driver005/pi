export module pi.platform.i_sleeper;

import std;
export import pi.support.abort_signal;

/** Interruptible delay; injected so retry backoff is testable without real waiting. */
export class ISleeper {
public:
    virtual ~ISleeper() = default;

    /** Waits for the duration. Returns false when the signal aborted before it elapsed. */
    virtual bool sleep(std::chrono::milliseconds duration,
                       const std::shared_ptr<AbortSignal>& signal) = 0;
};
