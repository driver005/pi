#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.faux_provider;
import pi.session.session_manager;
import pi.support.branch_summarizer;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.sequential_id_generator;

class BranchSummarizerTest : public testing::Test {
protected:
    BranchSummarizerTest() {
        SessionManagerOptions options;
        options.cwd = "/work";
        options.persist = false;
        m_session = std::make_unique<SessionManager>(options, m_files, m_clock, m_ids);
        EXPECT_TRUE(m_session->open().has_value());
        m_options.summarization.model.id = "m";
        m_options.summarization.model.api = "faux";
        m_options.summarization.model.contextWindow = 100000;
        m_options.summarization.streamFn = [this](const Model& model, const TranscriptContext& context,
                                                  const StreamOptions& request) {
            const auto& user = std::get<UserMessage>(context.messages.back());
            m_prompt = std::get<TextContent>(std::get<std::vector<UserContentBlock>>(user.content)[0]).text;
            m_request = request;
            return m_provider.stream(model, context, request);
        };
    }

    std::string append(const AgentMessage& message) {
        return *m_session->appendMessage(message);
    }

    AgentMessage user(const std::string& text) {
        UserMessage message;
        message.content = text;
        message.timestamp = m_clock.nowMs();
        return message;
    }

    AgentMessage assistant(const std::string& text, const std::string& readPath = "") {
        AssistantMessage message;
        message.api = "faux";
        message.provider = "p";
        message.model = "m";
        message.content.emplace_back(TextContent{text, std::nullopt});
        if (!readPath.empty()) {
            ToolCall call;
            call.id = "c";
            call.name = "read";
            call.arguments = Json{{"path", readPath}};
            message.content.emplace_back(call);
        }
        message.timestamp = m_clock.nowMs();
        return message;
    }

    FakeFileSystem m_files;
    FixedClock m_clock{1700000000000};
    SequentialIdGenerator m_ids{"sess"};
    InlineExecutor m_executor;
    RecordingSleeper m_sleeper;
    FauxProvider m_provider{m_executor, m_clock};
    SummaryGenerator m_generator{m_clock, m_ids, m_sleeper};
    BranchSummarizer m_summarizer{m_generator};
    std::unique_ptr<SessionManager> m_session;
    BranchSummaryOptions m_options;
    std::string m_prompt;
    StreamOptions m_request;
};

TEST_F(BranchSummarizerTest, CollectsAbandonedEntriesUpToCommonAncestor) {
    const std::string root = append(user("start"));
    const std::string a1 = append(assistant("a1"));
    const std::string b = append(user("branch step"));
    const std::string b2 = append(assistant("b2"));
    ASSERT_TRUE(m_session->branch(a1).has_value());
    const std::string c = append(user("other path"));
    const auto result = m_summarizer.collectEntries(*m_session, b2, c);
    EXPECT_EQ(result.commonAncestorId, a1);
    ASSERT_EQ(result.entries.size(), 2U);
    EXPECT_EQ(result.entries[0].id, b);
    EXPECT_EQ(result.entries[1].id, b2);
    EXPECT_TRUE(m_summarizer.collectEntries(*m_session, std::nullopt, c).entries.empty());
    (void)root;
}

TEST_F(BranchSummarizerTest, PrepareKeepsNewestMessagesWithinBudget) {
    append(user(std::string(400, 'a')));
    append(assistant(std::string(400, 'b')));
    append(user(std::string(400, 'c')));
    const auto prepared = m_summarizer.prepare(m_session->entries(), 250);
    ASSERT_EQ(prepared.messages.size(), 2U);
    EXPECT_EQ(std::get<std::string>(std::get<UserMessage>(prepared.messages[1]).content),
              std::string(400, 'c'));
    EXPECT_EQ(prepared.totalTokens, 200);
}

TEST_F(BranchSummarizerTest, SummarizeAddsPreambleAndFileLists) {
    append(user("explore"));
    append(assistant("looking", "src/x.cpp"));
    m_provider.enqueue(m_provider.textResponse("## Goal\nexplore"));
    const auto result = m_summarizer.summarize(m_session->entries(), m_options);
    ASSERT_TRUE(result.summary.has_value());
    EXPECT_EQ(result.summary->rfind("The user explored a different conversation branch", 0), 0U);
    EXPECT_NE(result.summary->find("## Goal\nexplore"), std::string::npos);
    EXPECT_NE(result.summary->find("<read-files>\nsrc/x.cpp\n</read-files>"), std::string::npos);
    EXPECT_EQ(result.readFiles, (std::vector<std::string>{"src/x.cpp"}));
    EXPECT_NE(m_prompt.find("Create a structured summary of this conversation branch"), std::string::npos);
    EXPECT_EQ(m_request.maxTokens, 4096);
    EXPECT_EQ(m_request.cacheRetention, "none");
}

TEST_F(BranchSummarizerTest, ReplaceInstructionsDropsDefaultPrompt) {
    append(user("explore"));
    m_provider.enqueue(m_provider.textResponse("x"));
    m_options.customInstructions = "Only list files";
    m_options.replaceInstructions = true;
    m_summarizer.summarize(m_session->entries(), m_options);
    EXPECT_EQ(m_prompt.find("Create a structured summary"), std::string::npos);
    EXPECT_NE(m_prompt.find("Only list files"), std::string::npos);
}

TEST_F(BranchSummarizerTest, EmptyBranchHasNothingToSummarize) {
    const auto result = m_summarizer.summarize({}, m_options);
    EXPECT_EQ(result.summary, "No content to summarize");
}

TEST_F(BranchSummarizerTest, AbortAndErrorAreReportedWithoutSummary) {
    append(user("explore"));
    AssistantMessage aborted = m_provider.textResponse("x");
    aborted.stopReason = StopReason::Aborted;
    m_provider.enqueue(aborted);
    EXPECT_TRUE(m_summarizer.summarize(m_session->entries(), m_options).aborted);
    AssistantMessage failed = m_provider.textResponse("x");
    failed.stopReason = StopReason::Error;
    failed.errorMessage = "400 bad";
    m_provider.enqueue(failed);
    const auto result = m_summarizer.summarize(m_session->entries(), m_options);
    EXPECT_EQ(result.error, "Branch summarization failed: 400 bad");
    EXPECT_FALSE(result.summary.has_value());
}
