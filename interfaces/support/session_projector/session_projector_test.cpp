#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.session_entry_codec;
import pi.support.session_projector;

class SessionProjectorTest : public testing::Test {
protected:
    SessionEntry entry(const std::string& json) { return m_codec.entryFromJson(Json::parse(json)); }

    SessionEntry user(const std::string& id, const std::optional<std::string>& parent, const std::string& text) {
        return entry(R"({"type":"message","id":")" + id + R"(","parentId":)" +
                     (parent ? "\"" + *parent + "\"" : std::string("null")) +
                     R"(,"timestamp":"2025-01-01T00:00:00.000Z","message":{"role":"user","content":")" + text +
                     R"(","timestamp":1}})");
    }

    SessionEntry assistant(const std::string& id, const std::string& parent, const std::string& text) {
        return entry(R"({"type":"message","id":")" + id + R"(","parentId":")" + parent +
                     R"(","timestamp":"2025-01-01T00:00:01.000Z","message":{"role":"assistant","content":[{"type":"text","text":")" +
                     text + R"("}],"api":"a","provider":"p","model":"m","usage":{"input":1,"output":1,"cacheRead":0,"cacheWrite":0,"totalTokens":2,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}},"stopReason":"stop","timestamp":2}})");
    }

    std::string text(const AgentMessage& message) {
        if (const auto* u = std::get_if<UserMessage>(&message)) {
            return std::get<std::string>(u->content);
        }
        if (const auto* a = std::get_if<AssistantMessage>(&message)) {
            return std::get<TextContent>(a->content[0]).text;
        }
        return "?";
    }

    SessionEntryCodec m_codec;
    SessionProjector m_projector;
};

TEST_F(SessionProjectorTest, PathFollowsParentsFromLeaf) {
    const std::vector<SessionEntry> entries = {user("a", std::nullopt, "one"), assistant("b", "a", "two"),
                                               user("c", "a", "branch")};
    const auto path = m_projector.buildPath(entries, std::string("c"));
    ASSERT_EQ(path.size(), 2U);
    EXPECT_EQ(path[0].id, "a");
    EXPECT_EQ(path[1].id, "c");
    EXPECT_TRUE(m_projector.buildPath(entries, std::nullopt).empty());
    EXPECT_EQ(m_projector.buildPath(entries, std::string("missing")).back().id, "c");
    EXPECT_EQ(m_projector.buildPathToEnd(entries).back().id, "c");
}

TEST_F(SessionProjectorTest, ContextMessagesAndSettings) {
    const std::vector<SessionEntry> entries = {
        entry(R"({"type":"thinking_level_change","id":"t","parentId":null,"timestamp":"2025-01-01T00:00:00.000Z","thinkingLevel":"high"})"),
        entry(R"({"type":"model_change","id":"m","parentId":"t","timestamp":"2025-01-01T00:00:00.000Z","provider":"anthropic","modelId":"claude"})"),
        user("a", "m", "hello"), assistant("b", "a", "hi")};
    const auto context = m_projector.context(entries, std::string("b"));
    ASSERT_EQ(context.messages.size(), 2U);
    EXPECT_EQ(text(context.messages[0]), "hello");
    EXPECT_EQ(context.thinkingLevel, "high");
    ASSERT_TRUE(context.model.has_value());
    // The assistant message's own provider/model wins as the latest.
    EXPECT_EQ(context.model->provider, "p");
    EXPECT_EQ(context.model->modelId, "m");
}

TEST_F(SessionProjectorTest, CompactionKeepsFromFirstKeptAndDropsSummarized) {
    const std::vector<SessionEntry> entries = {
        user("a", std::nullopt, "old"), assistant("b", "a", "old reply"), user("c", "b", "kept"),
        assistant("d", "c", "kept reply"),
        entry(R"({"type":"compaction","id":"k","parentId":"d","timestamp":"2025-01-01T00:00:02.000Z","summary":"SUM","firstKeptEntryId":"c","tokensBefore":99})"),
        user("e", "k", "after")};
    const auto context = m_projector.context(entries, std::string("e"));
    ASSERT_EQ(context.messages.size(), 4U);
    const auto* summary = std::get_if<CustomMessage>(&context.messages[0]);
    ASSERT_NE(summary, nullptr);
    EXPECT_EQ(summary->role, "compactionSummary");
    EXPECT_EQ(summary->data["summary"], "SUM");
    EXPECT_EQ(summary->data["tokensBefore"], 99);
    EXPECT_EQ(text(context.messages[1]), "kept");
    EXPECT_EQ(text(context.messages[2]), "kept reply");
    EXPECT_EQ(text(context.messages[3]), "after");
}

TEST_F(SessionProjectorTest, BranchSummaryAndCustomMessagesAreProjected) {
    const std::vector<SessionEntry> entries = {
        user("a", std::nullopt, "q"),
        entry(R"({"type":"branch_summary","id":"s","parentId":"a","timestamp":"2025-01-01T00:00:00.000Z","fromId":"zz","summary":"went elsewhere"})"),
        entry(R"({"type":"custom_message","id":"c","parentId":"s","timestamp":"2025-01-01T00:00:00.000Z","customType":"note","content":"injected","display":true,"details":{"k":1}})"),
        entry(R"({"type":"custom","id":"x","parentId":"c","timestamp":"2025-01-01T00:00:00.000Z","customType":"state","data":1})")};
    const auto context = m_projector.context(entries, std::string("x"));
    ASSERT_EQ(context.messages.size(), 3U);
    EXPECT_EQ(std::get<CustomMessage>(context.messages[1]).role, "branchSummary");
    const auto& custom = std::get<CustomMessage>(context.messages[2]);
    EXPECT_EQ(custom.role, "custom");
    EXPECT_EQ(custom.data["customType"], "note");
    EXPECT_EQ(custom.data["details"]["k"], 1);
}

TEST_F(SessionProjectorTest, ContextEditsReplaceOrOmitContent) {
    const std::vector<SessionEntry> entries = {
        user("a", std::nullopt, "secret"), assistant("b", "a", "reply"),
        entry(R"({"type":"context_edit","id":"e1","parentId":"b","timestamp":"2025-01-01T00:00:02.000Z","targetId":"a","replacement":{"content":"redacted"}})"),
        entry(R"({"type":"context_edit","id":"e2","parentId":"e1","timestamp":"2025-01-01T00:00:03.000Z","targetId":"b","replacement":null})")};
    const auto context = m_projector.context(entries, std::string("e2"));
    ASSERT_EQ(context.messages.size(), 1U);
    EXPECT_EQ(text(context.messages[0]), "redacted");
}

TEST_F(SessionProjectorTest, NullContentGetsDefaults) {
    const std::vector<SessionEntry> entries = {
        entry(R"({"type":"message","id":"a","parentId":null,"timestamp":"2025-01-01T00:00:00.000Z","message":{"role":"user","content":null,"timestamp":1}})")};
    const auto context = m_projector.context(entries, std::string("a"));
    ASSERT_EQ(context.messages.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<UserMessage>(context.messages[0]));
}

TEST_F(SessionProjectorTest, LatestCompactionLookup) {
    const std::vector<SessionEntry> entries = {
        entry(R"({"type":"compaction","id":"k1","parentId":null,"timestamp":"t","summary":"1","firstKeptEntryId":"k1","tokensBefore":1})"),
        entry(R"({"type":"compaction","id":"k2","parentId":"k1","timestamp":"t","summary":"2","firstKeptEntryId":"k2","tokensBefore":2})")};
    EXPECT_EQ(m_projector.latestCompaction(entries)->id, "k2");
    EXPECT_FALSE(m_projector.latestCompaction({}).has_value());
}
