export module pi.durable.i_submission;

import std;
export import pi.support.abort_signal;
export import pi.types.json;
export import pi.types.result;

/** Host object for one durably admitted submission. */
export class ISubmission {
public:
    virtual ~ISubmission() = default;

    virtual std::int64_t id() const = 0;
    virtual Result<Json> status() = 0;
    /** Blocks until the submission settles (`done` or `unanswered`); `cancel` aborts only this wait. */
    virtual Result<Json> wait(const AbortSignal* cancel = nullptr) = 0;
    /** "aborted", "already_placed" or "settled". */
    virtual Result<std::string> abort() = 0;
};
