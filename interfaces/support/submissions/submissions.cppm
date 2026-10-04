module;

#include <cstdint>

export module pi.support.submissions;

import std;
export import pi.durable.i_submissions;
export import pi.support.durable_session;
export import pi.support.inbox_boundary;
export import pi.support.json_waiters;
export import pi.support.submission_admission;
export import pi.support.submission_handle;
export import pi.types.resolved_settings;
export import pi.types.submission_draft;

/**
 * Admission, waits and withdrawal of the durable submissions of one harness. Port of packages/durable/src/harness/
 * submissions.ts.
 */
export class Submissions : public ISubmissions {
public:
    /** `resume` enables task scheduling: submitting or waiting asks for progress. */
    Submissions(DurableSession& session, std::function<std::int64_t()> now, std::function<ResolvedSettings()> settings,
                std::function<void()> resume)
        : m_session(session), m_now(std::move(now)), m_settings(std::move(settings)), m_resume(std::move(resume)) {
        if (auto line = m_session.subscribeCommitsOnLine([this](const Json& publication) { observe(publication); })) {
            m_lineSubscription = *line;
        }
        if (auto close = m_session.subscribeClose([this] {
                m_closed = true;
                m_waiters.rejectAll(Error{"harness_closed", "Harness is closed"});
            })) {
            m_closeSubscription = *close;
        }
    }

    Submissions(const Submissions&) = delete;
    Submissions& operator=(const Submissions&) = delete;

    ~Submissions() override {
        if (m_lineSubscription != 0) {
            m_session.unsubscribeCommitsOnLine(m_lineSubscription);
        }
        if (m_closeSubscription != 0) {
            m_session.unsubscribeClose(m_closeSubscription);
        }
    }

    /** Admits a submission in one commit; see SubmissionAdmission. */
    Result<std::shared_ptr<ISubmission>> submit(std::int64_t conversationId, const SubmissionDraft& draft) {
        m_resume();
        std::int64_t id = 0;
        auto committed = m_session.commit([&](Transaction& tx) -> Result<void> {
            const ResolvedSettings settings = m_settings();
            auto admitted = m_admission.admit(tx, conversationId, draft, m_now(), settings.steeringMode, settings.followUpMode);
            if (!admitted) {
                return std::unexpected(admitted.error());
            }
            id = *admitted;
            return {};
        });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        return std::shared_ptr<ISubmission>(std::make_shared<SubmissionHandle>(id, *this));
    }

    /** A handle for an existing submission; null when it does not exist. */
    Result<std::shared_ptr<ISubmission>> get(std::int64_t id) {
        std::optional<Json> record;
        auto read = m_session.readOnLine([&]() -> Result<void> {
            auto stored = m_session.storage().submission(id);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            record = *stored;
            return {};
        });
        if (!read) {
            return std::unexpected(read.error());
        }
        if (!record) {
            return std::shared_ptr<ISubmission>();
        }
        return std::shared_ptr<ISubmission>(std::make_shared<SubmissionHandle>(id, *this));
    }

    Result<Json> status(std::int64_t id) override {
        std::optional<Json> record;
        auto read = m_session.readOnLine([&]() -> Result<void> {
            auto stored = m_session.storage().submission(id);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            record = *stored;
            return {};
        });
        if (!read) {
            return std::unexpected(read.error());
        }
        if (!record) {
            return std::unexpected(Error{"durable_error", "Submission " + std::to_string(id) + " does not exist"});
        }
        return *record;
    }

    Result<Json> wait(std::int64_t id, const AbortSignal* cancel) override {
        m_resume();
        std::shared_ptr<WaiterSlot> slot;
        std::optional<Json> settled;
        // Check and register on the line so no settling publication falls between them.
        auto checked = m_session.readOnLine([&]() -> Result<void> {
            auto stored = m_session.storage().submission(id);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            if (!*stored) {
                return std::unexpected(Error{"durable_error", "Submission " + std::to_string(id) + " does not exist"});
            }
            if (isSettled(**stored)) {
                settled = **stored;
                return {};
            }
            // Close rejects registered waiters synchronously and may begin during the read.
            if (m_closed) {
                return std::unexpected(Error{"harness_closed", "Harness is closed"});
            }
            slot = m_waiters.add(id, cancel);
            return {};
        });
        if (!checked) {
            return std::unexpected(checked.error());
        }
        if (settled) {
            return *settled;
        }
        return m_waiters.await(slot);
    }

    /** Withdraws a queued submission and removes its inbox item; placed inputs and settled submissions are reported. */
    Result<std::string> abort(std::int64_t id, const std::optional<std::int64_t>& conversationId) override {
        std::string result = "not_found";
        auto committed = m_session.commit([&](Transaction& tx) -> Result<void> {
            auto record = tx.submission(id);
            if (!record) {
                return std::unexpected(record.error());
            }
            if (!*record || (conversationId && (*record)->at("conversationId").get<std::int64_t>() != *conversationId)) {
                result = "not_found";
                return {};
            }
            const std::string status = (*record)->at("status").get<std::string>();
            if (status == "queued") {
                if (auto settled = tx.settleSubmission(id, Json::object({{"status", "unanswered"}, {"reason", "aborted"}})); !settled) {
                    return settled;
                }
                result = "aborted";
                return m_boundary.removeItem(tx, (*record)->at("conversationId").get<std::int64_t>(), id);
            }
            result = status == "placed" ? "already_placed" : "settled";
            return {};
        });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        return result;
    }

private:
    bool isSettled(const Json& record) const {
        const Json& status = record.at("status");
        return status == "done" || status == "unanswered";
    }

    /** Line listener: settling publications release the waiters of their submissions. */
    void observe(const Json& publication) {
        for (const Json& change : publication.at("changes")) {
            if (change.at("type") == "submission" && isSettled(change.at("value"))) {
                m_waiters.resolve(change.at("value").at("id").get<std::int64_t>(), change.at("value"));
            }
        }
    }

    DurableSession& m_session;
    std::function<std::int64_t()> m_now;
    std::function<ResolvedSettings()> m_settings;
    std::function<void()> m_resume;
    SubmissionAdmission m_admission;
    InboxBoundary m_boundary;
    JsonWaiters m_waiters;
    std::atomic<bool> m_closed{false};
    std::int64_t m_lineSubscription = 0;
    std::int64_t m_closeSubscription = 0;
};
