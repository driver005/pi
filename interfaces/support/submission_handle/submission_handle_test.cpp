#include <gtest/gtest.h>

import std;
import pi.support.submission_handle;

class FakeSubmissions : public ISubmissions {
public:
    Result<Json> status(std::int64_t id) override {
        return Json::object({{"id", id}, {"status", "queued"}});
    }
    Result<Json> wait(std::int64_t id, const AbortSignal*) override {
        return Json::object({{"id", id}, {"status", "done"}});
    }
    Result<std::string> abort(std::int64_t, const std::optional<std::int64_t>&) override {
        return m_abortResult;
    }
    std::string m_abortResult = "aborted";
};

TEST(SubmissionHandleTest, DelegatesByIdAndTurnsNotFoundIntoAnError) {
    FakeSubmissions service;
    SubmissionHandle handle(5, service);
    EXPECT_EQ(handle.id(), 5);
    EXPECT_EQ(handle.status()->at("status"), "queued");
    EXPECT_EQ(handle.wait()->at("status"), "done");
    EXPECT_EQ(*handle.abort(), "aborted");
    service.m_abortResult = "already_placed";
    EXPECT_EQ(*handle.abort(), "already_placed");
    service.m_abortResult = "not_found";
    auto missing = handle.abort();
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message, "Submission 5 does not exist");
}
