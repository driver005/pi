module;

#include <cstdint>

export module pi.support.bound_submission;

import std;
export import pi.durable.i_submission;
export import pi.support.abort_link;
export import pi.types.task_invocation;

/**
 * A submission as a task invocation sees it: every operation first checks the invocation and runs under its signal, so it
 * fails once the invocation ends; the admitted work stays durable.
 */
export class BoundSubmission : public ISubmission {
public:
    BoundSubmission(std::shared_ptr<ISubmission> submission, std::shared_ptr<TaskInvocation> invocation)
        : m_submission(std::move(submission)), m_invocation(std::move(invocation)) {}

    std::int64_t id() const override {
        return m_submission->id();
    }

    Result<Json> status() override {
        if (auto live = check(); !live) {
            return std::unexpected(live.error());
        }
        return m_submission->status();
    }

    Result<Json> wait(const AbortSignal* cancel = nullptr) override {
        if (auto live = check(); !live) {
            return std::unexpected(live.error());
        }
        AbortLink link({&m_invocation->signal, cancel});
        return m_submission->wait(link.signal());
    }

    Result<std::string> abort() override {
        if (auto live = check(); !live) {
            return std::unexpected(live.error());
        }
        return m_submission->abort();
    }

private:
    Result<void> check() const {
        if (m_invocation->ended) {
            return std::unexpected(Error{"invocation_ended", "Task " + std::to_string(m_invocation->taskId) + " invocation has ended"});
        }
        return {};
    }

    std::shared_ptr<ISubmission> m_submission;
    std::shared_ptr<TaskInvocation> m_invocation;
};
