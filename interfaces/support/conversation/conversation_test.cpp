#include <gtest/gtest.h>

import std;
import pi.support.conversation;

class RecordingHost : public IConversationHost {
public:
    std::int64_t now() override {
        return 55;
    }
    Result<std::shared_ptr<const IAgent>> agent(std::int64_t) override {
        return std::shared_ptr<const IAgent>();
    }
    Result<void> configure(std::int64_t id, const Json& change) override {
        m_calls.push_back("configure " + std::to_string(id) + " " + change.dump());
        return {};
    }
    Result<std::shared_ptr<ISubmission>> submit(std::int64_t id, const SubmissionDraft& draft) override {
        m_calls.push_back("submit " + std::to_string(id) + " " + draft.type + " " + (draft.type == "write" ? draft.entry.dump() : std::string()));
        return std::shared_ptr<ISubmission>();
    }
    Result<std::int64_t> compact(std::int64_t id, const std::optional<std::string>& instructions) override {
        m_calls.push_back("compact " + std::to_string(id) + " " + instructions.value_or("-"));
        return 77;
    }
    Result<std::int64_t> commit(std::int64_t id, const std::function<Result<void>(Transaction&)>&) override {
        m_calls.push_back("commit " + std::to_string(id));
        return 1;
    }
    Result<Json> context(std::int64_t id) override {
        m_calls.push_back("context " + std::to_string(id));
        return Json::object();
    }
    Result<StoragePage> entries(std::int64_t id, const std::optional<std::int64_t>& min, const std::optional<std::int64_t>& max, std::size_t limit,
                                const std::optional<Json>&) override {
        m_calls.push_back("entries " + std::to_string(id) + " " + std::to_string(min.value_or(-1)) + " " + std::to_string(max.value_or(-1)) + " " +
                          std::to_string(limit));
        return StoragePage{};
    }
    Result<std::int64_t> fork(std::int64_t id, std::int64_t at, const ConversationCreateOptions&) override {
        m_calls.push_back("fork " + std::to_string(id) + " " + std::to_string(at));
        return 900;
    }
    Result<void> abort(std::int64_t id, bool background, const AbortSignal*) override {
        m_calls.push_back("abort " + std::to_string(id) + (background ? " bg" : ""));
        return {};
    }
    Result<void> waitForIdle(std::int64_t id, const AbortSignal*) override {
        m_calls.push_back("idle " + std::to_string(id));
        return {};
    }

    std::vector<std::string> m_calls;
};

TEST(ConversationTest, EveryOperationDelegatesWithTheConversationId) {
    RecordingHost host;
    Conversation conversation(3, host);
    EXPECT_EQ(conversation.id(), 3);
    EXPECT_TRUE(conversation.configure(Json::object({{"cwd", "/w"}})).has_value());
    EXPECT_TRUE(conversation.submit(SubmissionDraft{}).has_value());
    EXPECT_EQ(*conversation.compact(std::string("focus")), 77);
    EXPECT_EQ(*conversation.compact(), 77);
    EXPECT_TRUE(conversation.commit([](Transaction&) -> Result<void> { return {}; }).has_value());
    EXPECT_TRUE(conversation.context().has_value());
    EXPECT_TRUE(conversation.entries(std::nullopt, 9, 5).has_value());
    EXPECT_TRUE(conversation.abort(ConversationAbortOptions{true}).has_value());
    EXPECT_TRUE(conversation.waitForIdle().has_value());
    EXPECT_EQ(host.m_calls,
              std::vector<std::string>({"configure 3 {\"cwd\":\"/w\"}", "submit 3 input ", "compact 3 focus", "compact 3 -", "commit 3", "context 3",
                                        "entries 3 -1 9 5", "abort 3 bg", "idle 3"}));
}

TEST(ConversationTest, ForkReturnsAHandleOfTheNewConversation) {
    RecordingHost host;
    Conversation conversation(3, host);
    auto forked = conversation.fork(12);
    ASSERT_TRUE(forked.has_value());
    EXPECT_EQ((*forked)->id(), 900);
    EXPECT_EQ(host.m_calls[0], "fork 3 12");
}

TEST(ConversationTest, ResetAdmitsAHeadSelfWriteWithAnOptionalHandoff) {
    RecordingHost host;
    Conversation conversation(3, host);
    ASSERT_TRUE(conversation.reset(std::nullopt).has_value());
    ASSERT_TRUE(conversation.reset(std::string("carry on")).has_value());
    EXPECT_EQ(host.m_calls[0], R"(submit 3 write {"kind":"pi.reset","head":"self"})");
    EXPECT_EQ(host.m_calls[1],
              R"(submit 3 write {"kind":"pi.reset","head":"self","model":[{"role":"user","content":"carry on","timestamp":55}]})");
}
