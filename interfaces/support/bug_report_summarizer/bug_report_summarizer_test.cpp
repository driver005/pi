#include <gtest/gtest.h>

import std;
import pi.support.bug_report_summarizer;
import pi.testing.fixed_clock;
import pi.testing.recording_sleeper;
import pi.testing.sequential_id_generator;

class BugReportSummarizerTest : public testing::Test {
protected:
    BugReportSummarizerTest() {
        m_options.model.id = "m";
        m_options.model.provider = "p";
        m_options.model.api = "faux";
        m_options.model.contextWindow = 1000;
        m_options.model.maxTokens = 8192;
        m_options.streamFn = [this](const Model& model, const TranscriptContext& context, const StreamOptions& stream) {
            m_context = context;
            m_stream = stream;
            auto emitter = std::make_shared<AssistantMessageStream>([](const AssistantMessageEvent& event) { return event.type == AssistantEventType::Done || event.type == AssistantEventType::Error; }, [](const AssistantMessageEvent& event) { return *event.message; });
            AssistantMessage message = m_reply;
            message.provider = model.provider;
            AssistantMessageEvent event;
            event.type = message.stopReason == StopReason::Error ? AssistantEventType::Error : AssistantEventType::Done;
            event.reason = message.stopReason;
            event.message = std::make_shared<const AssistantMessage>(message);
            emitter->push(event);
            return emitter;
        };
        m_reply.stopReason = StopReason::Stop;
        TextContent text;
        text.text = "## What the user was doing\nA thing.";
        m_reply.content.push_back(text);
    }

    std::vector<AgentMessage> conversation(int count) {
        std::vector<AgentMessage> out;
        for (int i = 0; i < count; ++i) {
            UserMessage user;
            user.content = std::string(2000, 'x');
            user.timestamp = i;
            out.push_back(user);
        }
        return out;
    }

    std::string userText() {
        const UserMessage* user = nullptr;
        for (const Message& message : m_context.messages) {
            if (const auto* candidate = std::get_if<UserMessage>(&message)) {
                user = candidate;
            }
        }
        if (user == nullptr) {
            return "";
        }
        return std::get<TextContent>(std::get<std::vector<UserContentBlock>>(user->content).at(0)).text;
    }

    FixedClock m_clock;
    RecordingSleeper m_sleeper;
    SequentialIdGenerator m_ids;
    SummaryGenerator m_generator{m_clock, m_ids, m_sleeper};
    BugReportSummarizer m_summarizer{m_generator};
    SummarizationOptions m_options;
    TranscriptContext m_context;
    StreamOptions m_stream;
    AssistantMessage m_reply;
};

TEST_F(BugReportSummarizerTest, SendsTheConversationAndTheHintAndReturnsTheTrimmedReport) {
    const auto summary = m_summarizer.summarize(conversation(1), "  it crashed  ", m_options);
    ASSERT_TRUE(summary.has_value()) << summary.error().message;
    EXPECT_EQ(*summary, "## What the user was doing\nA thing.");
    const std::string prompt = userText();
    EXPECT_NE(prompt.find("<conversation>"), std::string::npos);
    EXPECT_NE(prompt.find("<user-report>\nit crashed\n</user-report>"), std::string::npos);
    EXPECT_NE(prompt.find("## Steps to reproduce"), std::string::npos);
    EXPECT_EQ(prompt.find("Note: only the last"), std::string::npos);
    EXPECT_EQ(m_stream.maxTokens, 4096);
    EXPECT_EQ(m_stream.cacheRetention, "none");
}

TEST_F(BugReportSummarizerTest, OnlyTheLastMessagesThatFitTheContextWindowAreSent) {
    const auto summary = m_summarizer.summarize(conversation(10), std::nullopt, m_options);
    ASSERT_TRUE(summary.has_value());
    const std::string prompt = userText();
    EXPECT_NE(prompt.find("Note: only the last "), std::string::npos);
    EXPECT_NE(prompt.find(" of 10 messages are shown."), std::string::npos);
    EXPECT_EQ(prompt.find("<user-report>"), std::string::npos);
}

TEST_F(BugReportSummarizerTest, FailuresAreReportedWithTheirReason) {
    m_reply.stopReason = StopReason::Error;
    m_reply.errorMessage = "rate limited";
    auto failed = m_summarizer.summarize(conversation(1), std::nullopt, m_options);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "Bug report summary failed: rate limited");

    m_reply.stopReason = StopReason::Aborted;
    failed = m_summarizer.summarize(conversation(1), std::nullopt, m_options);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, "aborted");

    m_reply.stopReason = StopReason::Stop;
    m_reply.content.clear();
    failed = m_summarizer.summarize(conversation(1), std::nullopt, m_options);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "Bug report summary was empty");

    ToolCall call;
    call.id = "c";
    call.name = "bash";
    m_reply.content.push_back(call);
    failed = m_summarizer.summarize(conversation(1), std::nullopt, m_options);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "Bug report summary attempted to call a tool");
}
