module;

#include <cstdint>

export module pi.support.submission_handle;

import std;
export import pi.durable.i_submission;
export import pi.durable.i_submissions;

/** Host object for one durably admitted submission; every operation delegates to the submission service by id. */
export class SubmissionHandle : public ISubmission {
public:
    SubmissionHandle(std::int64_t id, ISubmissions& submissions) : m_id(id), m_submissions(submissions) {}

    std::int64_t id() const override {
        return m_id;
    }

    Result<Json> status() override {
        return m_submissions.status(m_id);
    }

    Result<Json> wait(const AbortSignal* cancel = nullptr) override {
        return m_submissions.wait(m_id, cancel);
    }

    Result<std::string> abort() override {
        auto result = m_submissions.abort(m_id, std::nullopt);
        if (!result) {
            return result;
        }
        if (*result == "not_found") {
            return std::unexpected(Error{"durable_error", "Submission " + std::to_string(m_id) + " does not exist"});
        }
        return result;
    }

private:
    std::int64_t m_id;
    ISubmissions& m_submissions;
};
