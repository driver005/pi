#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.faux_provider;
import pi.support.compactor;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.sequential_id_generator;

class CompactorTest : public testing::Test {
protected:
    CompactorTest() {
        m_options.model.id = "m";
        m_options.model.api = "faux";
        m_options.streamFn = [this](const Model& model, const TranscriptContext& context,
                                    const StreamOptions& options) {
            m_prompts.push_back(promptOf(context));
            return m_provider.stream(model, context, options);
        };
    }

    std::string promptOf(const TranscriptContext& context) {
        const auto& user = std::get<UserMessage>(context.messages.back());
        return std::get<TextContent>(std::get<std::vector<UserContentBlock>>(user.content)[0]).text;
    }

    AgentMessage user(const std::string& text) {
        UserMessage message;
        message.content = text;
        return message;
    }

    AssistantMessage withUsage(const std::string& text, std::int64_t tokens) {
        AssistantMessage message = m_provider.textResponse(text);
        message.usage.totalTokens = tokens;
        message.usage.output = tokens;
        return message;
    }

    CompactionPreparation preparation() {
        CompactionPreparation out;
        out.firstKeptEntryId = "keep";
        out.tokensBefore = 500;
        out.messagesToSummarize = {user("history")};
        out.fileOps.read = {"a.txt", "b.txt"};
        out.fileOps.edited = {"b.txt"};
        out.settings.reserveTokens = 1000;
        return out;
    }

    FixedClock m_clock;
    InlineExecutor m_executor;
    SequentialIdGenerator m_ids;
    RecordingSleeper m_sleeper;
    FauxProvider m_provider{m_executor, m_clock};
    SummaryGenerator m_generator{m_clock, m_ids, m_sleeper};
    Compactor m_compactor{m_generator};
    SummarizationOptions m_options;
    std::vector<std::string> m_prompts;
};

TEST_F(CompactorTest, SummaryGetsFileListsAndDetails) {
    m_provider.enqueue(withUsage("SUMMARY", 40));
    const auto result = m_compactor.compact(preparation(), std::nullopt, m_options);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->summary,
              "SUMMARY\n\n<read-files>\na.txt\n</read-files>\n\n<modified-files>\nb.txt\n</modified-files>");
    EXPECT_EQ(result->firstKeptEntryId, "keep");
    EXPECT_EQ(result->tokensBefore, 500);
    EXPECT_GT(result->usage->totalTokens, 0);
    EXPECT_EQ(result->details["readFiles"], Json::array({"a.txt"}));
    EXPECT_EQ(result->details["modifiedFiles"], Json::array({"b.txt"}));
}

TEST_F(CompactorTest, SplitTurnMergesHistoryAndPrefixSummaries) {
    m_provider.enqueue(withUsage("HISTORY", 40));
    m_provider.enqueue(withUsage("PREFIX", 2));
    CompactionPreparation prep = preparation();
    prep.isSplitTurn = true;
    prep.turnPrefixMessages = {user("prefix")};
    const auto result = m_compactor.compact(prep, std::nullopt, m_options);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->summary.rfind("HISTORY\n\n---\n\n**Turn Context (split turn):**\n\nPREFIX", 0), 0U);
    EXPECT_GT(result->usage->totalTokens, 0);
    ASSERT_EQ(m_prompts.size(), 2U);
    EXPECT_NE(m_prompts[1].find("# Instructions"), std::string::npos);
}

TEST_F(CompactorTest, SplitTurnWithoutHistoryUsesPreviousSummary) {
    m_provider.enqueue(withUsage("PREFIX", 2));
    CompactionPreparation prep = preparation();
    prep.isSplitTurn = true;
    prep.messagesToSummarize.clear();
    prep.turnPrefixMessages = {user("prefix")};
    prep.previousSummary = "PREV";
    const auto result = m_compactor.compact(prep, std::nullopt, m_options);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->summary.rfind("PREV\n\n---\n\n", 0), 0U);
    EXPECT_EQ(m_prompts.size(), 1U);
}

TEST_F(CompactorTest, SummaryFailurePropagates) {
    AssistantMessage failed = m_provider.textResponse("x");
    failed.stopReason = StopReason::Error;
    failed.errorMessage = "400 nope";
    m_provider.enqueue(failed);
    const auto result = m_compactor.compact(preparation(), std::nullopt, m_options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Summarization failed: 400 nope");
}

TEST_F(CompactorTest, MissingFirstKeptIdIsAnError) {
    CompactionPreparation prep = preparation();
    prep.firstKeptEntryId.clear();
    EXPECT_FALSE(m_compactor.compact(prep, std::nullopt, m_options).has_value());
}
