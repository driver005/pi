#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.branch_navigator;
import pi.support.hook_bus;
import pi.testing.fake_settings_manager;
import pi.testing.recording_session_sink;
import pi.testing.session_harness;

class BranchNavigatorTest : public testing::Test {
protected:
    BranchNavigatorTest()
        : m_agent(m_harness.makeAgent()),
          m_refresher(*m_agent, m_harness.session()),
          m_generator(m_harness.clock(), m_harness.ids(), m_harness.sleeper()),
          m_summarizer(m_generator),
          m_navigator(*m_agent, m_harness.session(), m_settings, m_refresher, m_sink, m_summarizer,
                      [this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
                          return m_harness.provider().stream(model, context, options);
                      }) {
        buildTree();
    }

    std::string append(const AgentMessage& message) {
        return *m_harness.session().appendMessage(message);
    }

    AgentMessage user(const std::string& text) {
        UserMessage message;
        message.content = text;
        message.timestamp = ++m_stamp;
        return message;
    }

    AgentMessage assistant(const std::string& text) {
        AssistantMessage message;
        message.content.push_back(TextContent{text, std::nullopt});
        message.stopReason = StopReason::Stop;
        message.timestamp = ++m_stamp;
        return message;
    }

    /** root user -> a1 -> (branch A) u2 -> a2 ; the leaf ends on a2. */
    void buildTree() {
        m_root = append(user("start"));
        m_a1 = append(assistant("a1"));
        m_u2 = append(user("branch A question"));
        m_a2 = append(assistant("branch A answer"));
        m_refresher.refresh();
    }

    void on(const std::string& event, const std::function<Json(const Json&)>& handler) {
        m_bus.subscribe(event, [handler](const std::string&, const Json& payload) -> Result<Json> { return handler(payload); });
    }

    HookBus m_bus;
    PluginSessionEvents m_events{m_bus};
    SessionHarness m_harness;
    std::unique_ptr<IAgent> m_agent;
    FakeSettingsManager m_settings;
    SessionContextRefresher m_refresher;
    RecordingSessionSink m_sink;
    SummaryGenerator m_generator;
    BranchSummarizer m_summarizer;
    BranchNavigator m_navigator;
    std::string m_root;
    std::string m_a1;
    std::string m_u2;
    std::string m_a2;
    std::int64_t m_stamp = 1000;
};

TEST_F(BranchNavigatorTest, NavigatingToCurrentLeafIsANoOp) {
    const auto result = m_navigator.navigate(m_a2, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->cancelled);
    EXPECT_EQ(m_harness.session().leafId(), m_a2);
}

TEST_F(BranchNavigatorTest, NavigatingToAssistantEntryMovesLeafThere) {
    const auto result = m_navigator.navigate(m_a1, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(m_harness.session().leafId(), m_a1);
    EXPECT_FALSE(result->editorText.has_value());
    EXPECT_EQ(m_agent->messages().size(), 2U);
}

TEST_F(BranchNavigatorTest, NavigatingToUserMessageReturnsItsTextAndMovesToParent) {
    const auto result = m_navigator.navigate(m_u2, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->editorText, "branch A question");
    EXPECT_EQ(m_harness.session().leafId(), m_a1);
}

TEST_F(BranchNavigatorTest, NavigatingToRootUserMessageResetsLeaf) {
    const auto result = m_navigator.navigate(m_root, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->editorText, "start");
    EXPECT_FALSE(m_harness.session().leafId().has_value());
    EXPECT_TRUE(m_agent->messages().empty());
}

TEST_F(BranchNavigatorTest, SummarizingAttachesSummaryAtTargetAndLabelsIt) {
    m_harness.provider().enqueue(m_harness.provider().textResponse("## Goal\nbranch A work"));
    NavigateTreeOptions options;
    options.summarize = true;
    options.label = "branch-a";
    const auto result = m_navigator.navigate(m_a1, options);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->summaryEntry.has_value());
    EXPECT_EQ(result->summaryEntry->type, "branch_summary");
    EXPECT_EQ(result->summaryEntry->parentId, m_a1);
    EXPECT_NE(result->summaryEntry->body.value("summary", "").find("branch A work"), std::string::npos);
    EXPECT_EQ(m_harness.session().label(result->summaryEntry->id), "branch-a");
    EXPECT_EQ(m_harness.session().leafEntry()->type, "label");
    EXPECT_EQ(m_harness.session().leafEntry()->parentId, result->summaryEntry->id);
    EXPECT_FALSE(m_navigator.summarizing());
}

TEST_F(BranchNavigatorTest, LabelWithoutSummaryGoesOnTheTarget) {
    NavigateTreeOptions options;
    options.label = "mark";
    ASSERT_TRUE(m_navigator.navigate(m_a1, options).has_value());
    EXPECT_EQ(m_harness.session().label(m_a1), "mark");
}

TEST_F(BranchNavigatorTest, SummaryFailureAbortsNavigation) {
    AssistantMessage failed = m_harness.provider().textResponse("x");
    failed.stopReason = StopReason::Error;
    failed.errorMessage = "400 nope";
    m_harness.provider().enqueue(failed);
    NavigateTreeOptions options;
    options.summarize = true;
    const auto result = m_navigator.navigate(m_a1, options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Branch summarization failed: 400 nope");
    EXPECT_EQ(m_harness.session().leafId(), m_a2);
}

TEST_F(BranchNavigatorTest, AbortedSummaryCancelsNavigation) {
    m_harness.provider().enqueue([this](const TranscriptContext&, const StreamOptions&, const Model&) {
        m_navigator.abort();
        AssistantMessage aborted = m_harness.provider().textResponse("x");
        aborted.stopReason = StopReason::Aborted;
        return aborted;
    });
    NavigateTreeOptions options;
    options.summarize = true;
    const auto result = m_navigator.navigate(m_a1, options);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->cancelled);
    EXPECT_TRUE(result->aborted);
    EXPECT_EQ(m_harness.session().leafId(), m_a2);
}

TEST_F(BranchNavigatorTest, UnknownTargetIsAnError) {
    const auto result = m_navigator.navigate("nope", {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Entry nope not found");
}

TEST_F(BranchNavigatorTest, ListsUserMessagesForForking) {
    const auto messages = m_navigator.forkableMessages();
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_EQ(messages[0].entryId, m_root);
    EXPECT_EQ(messages[0].text, "start");
    EXPECT_EQ(messages[1].text, "branch A question");
}

TEST_F(BranchNavigatorTest, APluginCanCancelANavigation) {
    m_navigator.setEvents(&m_events);
    on("session_before_tree", [](const Json&) { return Json{{"cancel", true}}; });
    const auto result = m_navigator.navigate(m_a1, NavigateTreeOptions{});
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->cancelled);
    EXPECT_FALSE(result->aborted);
    EXPECT_EQ(m_harness.session().leafId(), m_a2);
    EXPECT_FALSE(m_navigator.summarizing());
}

TEST_F(BranchNavigatorTest, APluginCanSupplyTheSummaryAndTheLabel) {
    m_navigator.setEvents(&m_events);
    Json before;
    Json after;
    on("session_before_tree", [&before](const Json& payload) {
        before = payload;
        return Json{{"summary", Json{{"summary", "plugin summary"}, {"details", Json{{"k", 1}}}}}, {"label", "plugin-label"}};
    });
    on("session_tree", [&after](const Json& payload) {
        after = payload;
        return Json();
    });
    NavigateTreeOptions options;
    options.summarize = true;
    const auto result = m_navigator.navigate(m_a1, options);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->summaryEntry.has_value());
    EXPECT_EQ(result->summaryEntry->body.value("summary", ""), "plugin summary");
    EXPECT_EQ(result->summaryEntry->body.value("fromHook", false), true);
    EXPECT_EQ(m_harness.session().label(result->summaryEntry->id), "plugin-label");
    EXPECT_EQ(before["preparation"]["targetId"], m_a1);
    EXPECT_EQ(before["preparation"]["oldLeafId"], m_a2);
    EXPECT_EQ(before["preparation"]["userWantsSummary"], true);
    EXPECT_GT(before["preparation"]["entriesToSummarize"].size(), 0U);
    EXPECT_EQ(after["oldLeafId"], m_a2);
    EXPECT_EQ(after["fromExtension"], true);
    EXPECT_EQ(after["summaryEntry"]["id"], result->summaryEntry->id);
}

TEST_F(BranchNavigatorTest, APluginCanChangeTheSummarizationOptions) {
    m_navigator.setEvents(&m_events);
    on("session_before_tree", [](const Json&) { return Json{{"label", "L2"}}; });
    Json after;
    on("session_tree", [&after](const Json& payload) {
        after = payload;
        return Json();
    });
    ASSERT_TRUE(m_navigator.navigate(m_a1, NavigateTreeOptions{}).has_value());
    EXPECT_EQ(m_harness.session().label(m_a1), "L2");
    EXPECT_FALSE(after.contains("summaryEntry"));
    EXPECT_EQ(after["newLeafId"], m_harness.session().leafId());
}
