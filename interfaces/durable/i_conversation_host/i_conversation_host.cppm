module;

#include <cstdint>

export module pi.durable.i_conversation_host;

import std;
export import pi.durable.i_agent;
export import pi.durable.i_submission;
export import pi.support.abort_signal;
export import pi.support.transaction;
export import pi.types.conversation_abort_options;
export import pi.types.conversation_create_options;
export import pi.types.json;
export import pi.types.result;
export import pi.types.storage_page;
export import pi.types.submission_draft;

/** The harness services a conversation handle delegates to, addressed by conversation id. */
export class IConversationHost {
public:
    virtual ~IConversationHost() = default;

    virtual std::int64_t now() = 0;
    /** The conversation's agent resolved against the current registry snapshot and settings. */
    virtual Result<std::shared_ptr<const IAgent>> agent(std::int64_t conversationId) = 0;
    virtual Result<void> configure(std::int64_t conversationId, const Json& change) = 0;
    virtual Result<std::shared_ptr<ISubmission>> submit(std::int64_t conversationId, const SubmissionDraft& draft) = 0;
    /** Admits a manual compaction task and returns its id. */
    virtual Result<std::int64_t> compact(std::int64_t conversationId, const std::optional<std::string>& instructions) = 0;
    /** A session commit whose `createTask` defaults to the conversation; returns the commit sequence. */
    virtual Result<std::int64_t> commit(std::int64_t conversationId, const std::function<Result<void>(Transaction&)>& change) = 0;
    virtual Result<Json> context(std::int64_t conversationId) = 0;
    virtual Result<StoragePage> entries(std::int64_t conversationId, const std::optional<std::int64_t>& minEntryId,
                                        const std::optional<std::int64_t>& maxEntryId, std::size_t limit,
                                        const std::optional<Json>& cursor) = 0;
    /** Forks at a visible entry; returns the new conversation's id. */
    virtual Result<std::int64_t> fork(std::int64_t parentId, std::int64_t at, const ConversationCreateOptions& options) = 0;
    virtual Result<void> abort(std::int64_t conversationId, bool background, const AbortSignal* cancel) = 0;
    virtual Result<void> waitForIdle(std::int64_t conversationId, const AbortSignal* cancel) = 0;
};
