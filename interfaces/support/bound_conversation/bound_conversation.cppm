module;

#include <cstdint>

export module pi.support.bound_conversation;

import std;
export import pi.durable.i_conversation_handle;
export import pi.durable.i_conversation_host;
export import pi.support.abort_link;
export import pi.support.bound_submission;
export import pi.types.task_invocation;

/**
 * An existing conversation as a task or tool invocation sees it. Every operation, and every operation of a submission it
 * returns, first checks the invocation and runs under its signal, so it fails once the invocation ends; admitted work stays
 * durable. Port of boundConversation() in harness.ts.
 */
export class BoundConversation : public IConversationHandle {
public:
    BoundConversation(IConversationHost& host, std::int64_t id, std::shared_ptr<TaskInvocation> invocation)
        : m_host(host), m_id(id), m_invocation(std::move(invocation)) {}

    std::int64_t id() const override {
        return m_id;
    }

    Result<std::shared_ptr<ISubmission>> submit(const SubmissionDraft& draft) override {
        if (auto live = check(); !live) {
            return std::unexpected(live.error());
        }
        auto submission = m_host.submit(m_id, draft);
        if (!submission) {
            return std::unexpected(submission.error());
        }
        return std::shared_ptr<ISubmission>(std::make_shared<BoundSubmission>(*submission, m_invocation));
    }

    Result<void> abort(const ConversationAbortOptions& options = {}) override {
        if (auto live = check(); !live) {
            return live;
        }
        return m_host.abort(m_id, options.background, &m_invocation->signal);
    }

    Result<void> waitForIdle(const AbortSignal* cancel = nullptr) override {
        if (auto live = check(); !live) {
            return live;
        }
        AbortLink link({&m_invocation->signal, cancel});
        return m_host.waitForIdle(m_id, link.signal());
    }

private:
    Result<void> check() const {
        if (m_invocation->ended) {
            return std::unexpected(Error{"invocation_ended", "Task " + std::to_string(m_invocation->taskId) + " invocation has ended"});
        }
        return {};
    }

    IConversationHost& m_host;
    std::int64_t m_id;
    std::shared_ptr<TaskInvocation> m_invocation;
};
