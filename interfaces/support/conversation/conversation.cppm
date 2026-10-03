module;

#include <cstdint>

export module pi.support.conversation;

import std;
export import pi.durable.i_conversation_agent;
export import pi.durable.i_conversation_host;
export import pi.durable.i_submission;
export import pi.support.entry_kinds;
export import pi.types.conversation_abort_options;
export import pi.types.conversation_create_options;
export import pi.types.json;
export import pi.types.result;
export import pi.types.storage_page;
export import pi.types.submission_draft;

/**
 * A stateless handle for one conversation, bound to the harness that returned it. Compare handles by `id`. Port of the
 * `Conversation` interface of packages/durable/src/harness/types.ts.
 */
export class Conversation {
public:
    Conversation(std::int64_t id, IConversationHost& host) : m_id(id), m_host(host) {}

    std::int64_t id() const {
        return m_id;
    }

    /** The conversation's agent resolved with the current registry snapshot and settings. */
    Result<std::shared_ptr<const IConversationAgent>> agent() {
        return m_host.agent(m_id);
    }

    /** Applies an agent change (`model`, `thinkingLevel`, `extensions`, `tools`, `instructions`, `cwd`) in its own commit. */
    Result<void> configure(const Json& change) {
        return m_host.configure(m_id, change);
    }

    /**
     * Durably admits user input or a passive entry write. A busy conversation, or one with queued items, queues it in
     * `pi.inbox`; `whenBusy: "reject"` fails with `conversation_busy` instead and writes nothing.
     */
    Result<std::shared_ptr<ISubmission>> submit(const SubmissionDraft& draft) {
        return m_host.submit(m_id, draft);
    }

    /**
     * Admits a write of a `pi.reset` entry that starts a new context, carrying `handoff` as a user message when given.
     * While busy, it is placed at the next boundary.
     */
    Result<void> reset(const std::optional<std::string>& handoff) {
        Json entry = Json::object({{"kind", m_kinds.reset()}, {"head", "self"}});
        if (handoff) {
            entry["model"] = Json::array({Json::object({{"role", "user"}, {"content", *handoff}, {"timestamp", m_host.now()}})});
        }
        SubmissionDraft draft;
        draft.type = "write";
        draft.entry = entry;
        auto submitted = m_host.submit(m_id, draft);
        return submitted ? Result<void>() : std::unexpected(submitted.error());
    }

    /**
     * Admits a manual compaction task and returns its id. It summarizes while the conversation keeps working and places its
     * summary through a write submission: at once when idle, otherwise at the next boundary.
     */
    Result<std::int64_t> compact(const std::optional<std::string>& instructions = std::nullopt) {
        return m_host.compact(m_id, instructions);
    }

    /** A session commit whose `createTask` defaults to this conversation; returns the commit sequence. */
    Result<std::int64_t> commit(const std::function<Result<void>(Transaction&)>& change) {
        return m_host.commit(m_id, change);
    }

    /** The committed raw active transcript and model context: `{head, entries, contributions, messages}`. */
    Result<Json> context() {
        return m_host.context(m_id);
    }

    /** Newest-first fork-aware history of this conversation within inclusive entry id bounds. */
    Result<StoragePage> entries(const std::optional<std::int64_t>& minEntryId, const std::optional<std::int64_t>& maxEntryId,
                                std::size_t limit, const std::optional<Json>& cursor = std::nullopt) {
        return m_host.entries(m_id, minEntryId, maxEntryId, limit, cursor);
    }

    /** Forks at a visible entry with explicitly selected ownership. */
    Result<std::shared_ptr<Conversation>> fork(std::int64_t at, const ConversationCreateOptions& options = {}) {
        auto id = m_host.fork(m_id, at, options);
        if (!id) {
            return std::unexpected(id.error());
        }
        return std::make_shared<Conversation>(*id, m_host);
    }

    /**
     * Withdraws queued inputs (queued writes stay), marks every live non-background task of the ordinary ownership scope,
     * signals them, and returns once the scope is idle. Background subtrees survive unless `background` is set.
     */
    Result<void> abort(const ConversationAbortOptions& options = {}, const AbortSignal* cancel = nullptr) {
        return m_host.abort(m_id, options.background, cancel);
    }

    /**
     * Blocks until the ordinary ownership scope has no live non-background task: this conversation and the conversations
     * owned, transitively, by its non-background tasks.
     */
    Result<void> waitForIdle(const AbortSignal* cancel = nullptr) {
        return m_host.waitForIdle(m_id, cancel);
    }

private:
    std::int64_t m_id;
    IConversationHost& m_host;
    EntryKinds m_kinds;
};
