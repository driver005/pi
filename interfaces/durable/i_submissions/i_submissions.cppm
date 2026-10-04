module;

#include <cstdint>

export module pi.durable.i_submissions;

import std;
export import pi.support.abort_signal;
export import pi.types.json;
export import pi.types.result;

/** The admitted-submission service handles delegate to. */
export class ISubmissions {
public:
    virtual ~ISubmissions() = default;

    virtual Result<Json> status(std::int64_t id) = 0;
    /** Blocks until the submission settles (`done` or `unanswered`) and returns its record. */
    virtual Result<Json> wait(std::int64_t id, const AbortSignal* cancel) = 0;
    /** "aborted", "already_placed", "settled" or "not_found" (also for another conversation than `conversationId`). */
    virtual Result<std::string> abort(std::int64_t id, const std::optional<std::int64_t>& conversationId) = 0;
};
