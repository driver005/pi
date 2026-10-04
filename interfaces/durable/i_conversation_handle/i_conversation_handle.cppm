export module pi.durable.i_conversation_handle;

import std;
export import pi.durable.i_submission;
export import pi.support.abort_signal;
export import pi.types.conversation_abort_options;
export import pi.types.result;
export import pi.types.submission_draft;

/** Invocation-bound conversation operations for tasks and tools; they fail once the invocation ends. */
export class IConversationHandle {
public:
    virtual ~IConversationHandle() = default;

    virtual std::int64_t id() const = 0;
    virtual Result<std::shared_ptr<ISubmission>> submit(const SubmissionDraft& draft) = 0;
    /** Withdraws queued inputs, aborts the ordinary ownership scope, and waits until it is idle. */
    virtual Result<void> abort(const ConversationAbortOptions& options = {}) = 0;
    virtual Result<void> waitForIdle(const AbortSignal* cancel = nullptr) = 0;
};
