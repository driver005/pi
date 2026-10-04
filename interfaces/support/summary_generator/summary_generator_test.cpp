#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.faux_provider;
import pi.support.summary_generator;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.sequential_id_generator;

class SummaryGeneratorTest : public testing::Test {
protected:
    SummaryGeneratorTest() {
        m_options.model.id = "m";
        m_options.model.provider = "p";
        m_options.model.api = "faux";
        m_options.model.maxTokens = 0;
        m_options.streamFn = [this](const Model& model, const TranscriptContext& context,
                                    const StreamOptions& options) {
            m_requests.push_back(options);
            m_contexts.push_back(context);
            return m_provider.stream(model, context, options);
        };
    }

    std::vector<AgentMessage> conversation() {
        UserMessage user;
        user.content = std::string("fix the bug");
        return {user};
    }

    std::string promptOf(std::size_t index) {
        const auto& user = std::get<UserMessage>(m_contexts[index].messages.back());
        return std::get<TextContent>(std::get<std::vector<UserContentBlock>>(user.content)[0]).text;
    }

    AssistantMessage failing(StopReason reason, const std::string& text) {
        AssistantMessage message = m_provider.textResponse("partial");
        message.stopReason = reason;
        message.errorMessage = text;
        return message;
    }

    FixedClock m_clock;
    InlineExecutor m_executor;
    SequentialIdGenerator m_ids{"sess"};
    RecordingSleeper m_sleeper;
    FauxProvider m_provider{m_executor, m_clock};
    SummaryGenerator m_generator{m_clock, m_ids, m_sleeper};
    SummarizationOptions m_options;
    std::vector<StreamOptions> m_requests;
    std::vector<TranscriptContext> m_contexts;
};

TEST_F(SummaryGeneratorTest, GeneratesSummaryFromWrappedConversation) {
    m_provider.enqueue(m_provider.textResponse("## Goal\nfix"));
    const auto result = m_generator.generate(conversation(), 1000, std::nullopt, std::nullopt, m_options);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text, "## Goal\nfix");
    ASSERT_EQ(m_requests.size(), 1U);
    EXPECT_EQ(m_requests[0].maxTokens, 800);
    EXPECT_EQ(m_requests[0].cacheRetention, "none");
    EXPECT_EQ(m_requests[0].sessionId, "sess1");
    const std::string prompt = promptOf(0);
    EXPECT_EQ(prompt.rfind("<conversation>\n[User]: fix the bug\n</conversation>\n\n", 0), 0U);
    EXPECT_NE(prompt.find("Create a structured context checkpoint summary"), std::string::npos);
    EXPECT_EQ(prompt.find("<previous-summary>"), std::string::npos);
}

TEST_F(SummaryGeneratorTest, PreviousSummaryUsesUpdatePromptAndFocus) {
    m_provider.enqueue(m_provider.textResponse("merged"));
    m_options.stream.sessionId = "keep";
    const auto result = m_generator.generate(conversation(), 1000, std::string("auth only"),
                                             std::string("old"), m_options);
    ASSERT_TRUE(result.has_value());
    const std::string prompt = promptOf(0);
    EXPECT_NE(prompt.find("<previous-summary>\nold\n</previous-summary>"), std::string::npos);
    EXPECT_NE(prompt.find("Update the existing structured summary"), std::string::npos);
    EXPECT_NE(prompt.find("Additional focus: auth only"), std::string::npos);
    EXPECT_EQ(m_requests[0].sessionId, "keep");
}

TEST_F(SummaryGeneratorTest, ModelMaxTokensCapsBudgetAndReasoningFollowsLevel) {
    m_options.model.maxTokens = 500;
    m_options.model.reasoning = true;
    m_options.thinkingLevel = ThinkingLevel::High;
    m_provider.enqueue(m_provider.textResponse("s"));
    ASSERT_TRUE(m_generator.generate(conversation(), 1000, std::nullopt, std::nullopt, m_options));
    EXPECT_EQ(m_requests[0].maxTokens, 500);
    EXPECT_EQ(m_requests[0].reasoning, ThinkingLevel::High);
}

TEST_F(SummaryGeneratorTest, ProviderErrorBecomesError) {
    m_provider.enqueue(failing(StopReason::Error, "400 bad"));
    const auto result = m_generator.generate(conversation(), 1000, std::nullopt, std::nullopt, m_options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Summarization failed: 400 bad");
}

TEST_F(SummaryGeneratorTest, LengthStopAndToolCallsAreRejected) {
    m_provider.enqueue(failing(StopReason::Length, ""));
    auto length = m_generator.generate(conversation(), 1000, std::nullopt, std::nullopt, m_options);
    ASSERT_FALSE(length.has_value());
    EXPECT_EQ(length.error().message,
              "Summarization failed: generation hit the token cap and the summary is incomplete");
    m_provider.enqueue(m_provider.toolCallResponse("read", Json::object(), "t1"));
    auto tool = m_generator.generate(conversation(), 1000, std::nullopt, std::nullopt, m_options);
    ASSERT_FALSE(tool.has_value());
    EXPECT_EQ(tool.error().message, "Summarization attempted to call a tool");
}

TEST_F(SummaryGeneratorTest, TransientFailureIsRetriedPerPolicy) {
    m_options.retry.enabled = true;
    m_options.retry.maxRetries = 2;
    m_options.retry.baseDelayMs = 10;
    m_provider.enqueue(failing(StopReason::Error, "503 overloaded"));
    m_provider.enqueue(m_provider.textResponse("ok"));
    const auto result = m_generator.generate(conversation(), 1000, std::nullopt, std::nullopt, m_options);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text, "ok");
    EXPECT_EQ(m_requests.size(), 2U);
    EXPECT_EQ(m_sleeper.delays(), (std::vector<std::int64_t>{10}));
}

TEST_F(SummaryGeneratorTest, AbortedResponseReportsAborted) {
    m_provider.enqueue(failing(StopReason::Aborted, ""));
    const auto result = m_generator.generate(conversation(), 1000, std::nullopt, std::nullopt, m_options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "aborted");
}

TEST_F(SummaryGeneratorTest, TurnPrefixUsesHalfBudgetAndOwnPrompt) {
    m_provider.enqueue(m_provider.textResponse("prefix"));
    const auto result = m_generator.generateTurnPrefix(conversation(), 1000, m_options);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(m_requests[0].maxTokens, 500);
    const std::string prompt = promptOf(0);
    EXPECT_EQ(prompt.rfind("# Conversation\n[User]: fix the bug\n\n# Instructions\n", 0), 0U);
    EXPECT_NE(prompt.find("## Original Request"), std::string::npos);
}

TEST_F(SummaryGeneratorTest, MissingStreamFunctionIsAnError) {
    m_options.streamFn = nullptr;
    const auto result = m_generator.generate(conversation(), 1000, std::nullopt, std::nullopt, m_options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Summarization failed: No stream function available for summarization");
}
