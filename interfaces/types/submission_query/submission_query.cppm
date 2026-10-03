export module pi.types.submission_query;

import std;

/** Optional filters of a submission scan. */
export struct SubmissionQuery {
    std::optional<std::int64_t> conversationId;
    /** queued, placed, done or unanswered. */
    std::optional<std::string> status;
};
