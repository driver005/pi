#include <gtest/gtest.h>

import std;
import pi.support.bound_submission;

class StubSubmission : public ISubmission {
public:
    std::int64_t id() const override {
        return 3;
    }
    Result<Json> status() override {
        return Json::object({{"status", "queued"}});
    }
    Result<Json> wait(const AbortSignal* cancel) override {
        // Blocks until the supplied signal aborts, like a wait on an unsettled submission.
        (void)WaitGate().wait({cancel});
        return std::unexpected(Error{"aborted", "The operation was aborted"});
    }
    Result<std::string> abort() override {
        return std::string("aborted");
    }
};

TEST(BoundSubmissionTest, DelegatesWhileTheInvocationLives) {
    auto invocation = std::make_shared<TaskInvocation>();
    BoundSubmission bound(std::make_shared<StubSubmission>(), invocation);
    EXPECT_EQ(bound.id(), 3);
    EXPECT_EQ(bound.status()->at("status"), "queued");
    EXPECT_EQ(*bound.abort(), "aborted");
}

TEST(BoundSubmissionTest, FailsOnceTheInvocationEnded) {
    auto invocation = std::make_shared<TaskInvocation>();
    invocation->taskId = 12;
    BoundSubmission bound(std::make_shared<StubSubmission>(), invocation);
    invocation->ended = true;
    EXPECT_EQ(bound.status().error().code, "invocation_ended");
    EXPECT_FALSE(bound.wait().has_value());
    EXPECT_FALSE(bound.abort().has_value());
}

TEST(BoundSubmissionTest, WaitsRunUnderTheInvocationSignal) {
    auto invocation = std::make_shared<TaskInvocation>();
    BoundSubmission bound(std::make_shared<StubSubmission>(), invocation);
    std::atomic<bool> finished{false};
    std::thread waiter([&] {
        EXPECT_FALSE(bound.wait().has_value());
        finished = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(finished);
    invocation->signal.abort();
    waiter.join();
    EXPECT_TRUE(finished);
}
