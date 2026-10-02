#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.cut_point_finder;

class CutPointFinderTest : public testing::Test {
protected:
    ProjectedSessionEntry add(const std::string& id, AgentMessage message, bool omitted = false) {
        ProjectedSessionEntry entry;
        entry.sourceEntry.id = id;
        entry.sourceEntry.type = "message";
        entry.sourceEntry.body = Json{{"type", "message"}, {"id", id}, {"message", m_codec.toJson(message)}};
        if (!omitted) {
            entry.messages.push_back(std::move(message));
        }
        return entry;
    }

    AgentMessage user(std::size_t chars) {
        UserMessage message;
        message.content = std::string(chars, 'u');
        return message;
    }

    AgentMessage assistant(std::size_t chars, bool toolCall = false) {
        AssistantMessage message;
        message.content.push_back(TextContent{std::string(chars, 'a'), std::nullopt});
        if (toolCall) {
            ToolCall call;
            call.id = "t";
            call.name = "read";
            message.content.push_back(call);
        }
        return message;
    }

    AgentMessage toolResult(std::size_t chars) {
        ToolResultMessage message;
        message.toolCallId = "t";
        message.content.push_back(TextContent{std::string(chars, 'r'), std::nullopt});
        return message;
    }

    AgentMessageCodec m_codec;
    CutPointFinder m_finder;
};

TEST_F(CutPointFinderTest, KeepsRecentTokensAtTurnBoundary) {
    std::vector<ProjectedSessionEntry> entries = {add("1", user(400)), add("2", assistant(400)),
                                                  add("3", user(400)), add("4", assistant(400))};
    const auto cut = m_finder.find(entries, 0, entries.size(), 50);
    EXPECT_EQ(cut.firstKeptEntryIndex, 3U);
    EXPECT_TRUE(cut.isSplitTurn);
    EXPECT_EQ(cut.turnStartIndex, 2);
    const auto wide = m_finder.find(entries, 0, entries.size(), 150);
    EXPECT_EQ(wide.firstKeptEntryIndex, 2U);
    EXPECT_FALSE(wide.isSplitTurn);
    EXPECT_EQ(wide.turnStartIndex, -1);
}

TEST_F(CutPointFinderTest, NeverCutsAtToolResults) {
    std::vector<ProjectedSessionEntry> entries = {add("1", user(40)), add("2", assistant(40, true)),
                                                  add("3", toolResult(4000))};
    const auto cut = m_finder.find(entries, 0, entries.size(), 100);
    EXPECT_EQ(cut.firstKeptEntryIndex, 1U);
    EXPECT_TRUE(cut.isSplitTurn);
    EXPECT_EQ(cut.turnStartIndex, 0);
}

TEST_F(CutPointFinderTest, SmallConversationKeepsFromFirstCutPoint) {
    std::vector<ProjectedSessionEntry> entries = {add("1", user(40)), add("2", assistant(40))};
    const auto cut = m_finder.find(entries, 0, entries.size(), 100000);
    EXPECT_EQ(cut.firstKeptEntryIndex, 0U);
    EXPECT_FALSE(cut.isSplitTurn);
}

TEST_F(CutPointFinderTest, NoCutPointsReturnsStart) {
    std::vector<ProjectedSessionEntry> entries = {add("1", toolResult(40))};
    const auto cut = m_finder.find(entries, 0, entries.size(), 1);
    EXPECT_EQ(cut.firstKeptEntryIndex, 0U);
    EXPECT_EQ(cut.turnStartIndex, -1);
    EXPECT_FALSE(cut.isSplitTurn);
}

TEST_F(CutPointFinderTest, AdjacentInvisibleEntriesJoinKeptRange) {
    ProjectedSessionEntry label;
    label.sourceEntry.id = "l";
    label.sourceEntry.type = "label";
    std::vector<ProjectedSessionEntry> entries = {add("1", user(400)), add("2", assistant(400)),
                                                  label, add("3", user(400))};
    const auto cut = m_finder.find(entries, 0, entries.size(), 50);
    EXPECT_EQ(cut.firstKeptEntryIndex, 2U);
}

TEST_F(CutPointFinderTest, OmittedRecoveryAttemptMovesCutPastIt) {
    std::vector<ProjectedSessionEntry> entries = {add("1", user(400)), add("2", user(400)),
                                                  add("3", assistant(40), true)};
    const auto cut = m_finder.find(entries, 0, entries.size(), 50);
    EXPECT_EQ(cut.firstKeptEntryIndex, 2U);
}
