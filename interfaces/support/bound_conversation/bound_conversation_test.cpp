#include <gtest/gtest.h>

import std;
import pi.support.bound_conversation;

class StubSubmission : public ISubmission {
public:
    std::int64_t id() const override {
        return 9;
    }
    Result<Json> status() override {
        return Json::object();
    }
    Result<Json> wait(const AbortSignal*) override {
        return Json::object();
    }
    Result<std::string> abort() override {
        return std::string("aborted");
    }
};

class StubHost : public IConversationHost {
public:
    std::int64_t now() override {
        return 0;
    }
    Result<std::shared_ptr<const IAgent>> agent(std::int64_t) override {
        return std::shared_ptr<const IAgent>();
    }
    Result<void> configure(std::int64_t, const Json&) override {
        return {};
    }
    Result<std::shared_ptr<ISubmission>> submit(std::int64_t id, const SubmissionDraft&) override {
        m_submitted = id;
        return std::shared_ptr<ISubmission>(std::make_shared<StubSubmission>());
    }
    Result<std::int64_t> compact(std::int64_t, const std::optional<std::string>&) override {
        return 0;
    }
    Result<std::int64_t> commit(std::int64_t, const std::function<Result<void>(Transaction&)>&) override {
        return 0;
    }
    Result<Json> context(std::int64_t) override {
        return Json::object();
    }
    Result<StoragePage> entries(std::int64_t, const std::optional<std::int64_t>&, const std::optional<std::int64_t>&, std::size_t,
                                const std::optional<Json>&) override {
        return StoragePage{};
    }
    Result<std::int64_t> fork(std::int64_t, std::int64_t, const ConversationCreateOptions&) override {
        return 0;
    }
    Result<void> abort(std::int64_t id, bool background, const AbortSignal*) override {
        m_aborted = id;
        m_background = background;
        return {};
    }
    Result<void> waitForIdle(std::int64_t id, const AbortSignal* cancel) override {
        m_idleWaits.push_back(id);
        m_idleHadSignal = cancel != nullptr;
        return {};
    }

    std::int64_t m_submitted = 0;
    std::int64_t m_aborted = 0;
    bool m_background = false;
    std::vector<std::int64_t> m_idleWaits;
    bool m_idleHadSignal = false;
};

TEST(BoundConversationTest, DelegatesAndWrapsSubmissionsWhileTheInvocationLives) {
    StubHost host;
    auto invocation = std::make_shared<TaskInvocation>();
    BoundConversation conversation(host, 4, invocation);
    EXPECT_EQ(conversation.id(), 4);
    auto submission = conversation.submit(SubmissionDraft{});
    ASSERT_TRUE(submission.has_value());
    EXPECT_EQ(host.m_submitted, 4);
    EXPECT_EQ((*submission)->id(), 9);
    ConversationAbortOptions options;
    options.background = true;
    ASSERT_TRUE(conversation.abort(options).has_value());
    EXPECT_EQ(host.m_aborted, 4);
    EXPECT_TRUE(host.m_background);
    ASSERT_TRUE(conversation.waitForIdle().has_value());
    EXPECT_EQ(host.m_idleWaits, std::vector<std::int64_t>({4}));
    // The idle wait runs under a signal linked to the invocation's.
    EXPECT_TRUE(host.m_idleHadSignal);
}

TEST(BoundConversationTest, EverythingFailsOnceTheInvocationEnded) {
    StubHost host;
    auto invocation = std::make_shared<TaskInvocation>();
    BoundConversation conversation(host, 4, invocation);
    auto submission = conversation.submit(SubmissionDraft{});
    ASSERT_TRUE(submission.has_value());
    invocation->ended = true;
    EXPECT_EQ(conversation.submit(SubmissionDraft{}).error().code, "invocation_ended");
    EXPECT_FALSE(conversation.abort().has_value());
    EXPECT_FALSE(conversation.waitForIdle().has_value());
    EXPECT_FALSE((*submission)->status().has_value());
    EXPECT_EQ(host.m_submitted, 4);
    EXPECT_EQ(host.m_idleWaits.size(), 0u);
}
