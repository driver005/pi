#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.compaction_preparer;

class CompactionPreparerTest : public testing::Test {
protected:
    SessionEntry message(const std::string& id, AgentMessage content) {
        SessionEntry entry;
        entry.type = "message";
        entry.id = id;
        entry.parentId = m_last;
        entry.timestamp = "2025-01-01T00:00:00.000Z";
        entry.body = Json{{"type", "message"},
                          {"id", id},
                          {"parentId", m_last ? Json(*m_last) : Json(nullptr)},
                          {"timestamp", entry.timestamp},
                          {"message", m_codec.toJson(content)}};
        m_last = id;
        return entry;
    }

    SessionEntry compaction(const std::string& id, const std::string& firstKept) {
        SessionEntry entry;
        entry.type = "compaction";
        entry.id = id;
        entry.parentId = m_last;
        entry.timestamp = "2025-01-01T00:00:00.000Z";
        entry.body = Json{{"type", "compaction"},
                          {"id", id},
                          {"parentId", m_last ? Json(*m_last) : Json(nullptr)},
                          {"timestamp", entry.timestamp},
                          {"summary", "old summary"},
                          {"firstKeptEntryId", firstKept},
                          {"tokensBefore", 10},
                          {"details", Json{{"readFiles", {"seen.txt"}}, {"modifiedFiles", {"made.txt"}}}}};
        m_last = id;
        return entry;
    }

    AgentMessage user(std::size_t chars) {
        UserMessage content;
        content.content = std::string(chars, 'u');
        return content;
    }

    AgentMessage assistant(std::size_t chars, const std::string& readPath = "") {
        AssistantMessage content;
        content.content.push_back(TextContent{std::string(chars, 'a'), std::nullopt});
        if (!readPath.empty()) {
            ToolCall call;
            call.id = "c";
            call.name = "read";
            call.arguments = Json{{"path", readPath}};
            content.content.push_back(call);
        }
        content.stopReason = StopReason::Stop;
        return content;
    }

    CompactionSettings settings(std::int64_t keep) {
        CompactionSettings out;
        out.keepRecentTokens = keep;
        return out;
    }

    std::optional<std::string> m_last;
    AgentMessageCodec m_codec;
    CompactionPreparer m_preparer;
};

TEST_F(CompactionPreparerTest, SummarizesOlderTurnsAndKeepsRecent) {
    const std::vector<SessionEntry> path = {message("a", user(400)), message("b", assistant(400, "x.txt")),
                                            message("c", user(400)), message("d", assistant(400))};
    const auto prepared = m_preparer.prepare(path, settings(150));
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(prepared->firstKeptEntryId, "c");
    EXPECT_FALSE(prepared->isSplitTurn);
    EXPECT_EQ(prepared->messagesToSummarize.size(), 2U);
    EXPECT_FALSE(prepared->previousSummary.has_value());
    EXPECT_EQ(prepared->fileOps.read, std::set<std::string>{"x.txt"});
    EXPECT_GT(prepared->tokensBefore, 0);
}

TEST_F(CompactionPreparerTest, SplitTurnCollectsPrefixSeparately) {
    const std::vector<SessionEntry> path = {message("a", user(400)), message("b", assistant(400)),
                                            message("c", user(400)), message("d", assistant(400, "p.txt")),
                                            message("e", assistant(400))};
    const auto prepared = m_preparer.prepare(path, settings(50));
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(prepared->firstKeptEntryId, "e");
    EXPECT_TRUE(prepared->isSplitTurn);
    EXPECT_EQ(prepared->messagesToSummarize.size(), 2U);
    EXPECT_EQ(prepared->turnPrefixMessages.size(), 2U);
    EXPECT_EQ(prepared->fileOps.read, std::set<std::string>{"p.txt"});
}

TEST_F(CompactionPreparerTest, PreviousCompactionProvidesSummaryAndFileDetails) {
    const std::vector<SessionEntry> path = {message("a", user(400)), compaction("k", "a"),
                                            message("b", user(400)), message("c", assistant(400)),
                                            message("d", user(400)), message("e", assistant(400))};
    const auto prepared = m_preparer.prepare(path, settings(150));
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(prepared->previousSummary, "old summary");
    EXPECT_EQ(prepared->fileOps.read, std::set<std::string>{"seen.txt"});
    EXPECT_EQ(prepared->fileOps.edited, std::set<std::string>{"made.txt"});
}

TEST_F(CompactionPreparerTest, NothingToDoWhenPathEndsInCompactionOrIsEmpty) {
    EXPECT_FALSE(m_preparer.prepare({}, settings(100)).has_value());
    const std::vector<SessionEntry> path = {message("a", user(400)), compaction("k", "a")};
    EXPECT_FALSE(m_preparer.prepare(path, settings(100)).has_value());
}

TEST_F(CompactionPreparerTest, ShortSessionHasNothingToSummarize) {
    const std::vector<SessionEntry> path = {message("a", user(40)), message("b", assistant(40))};
    EXPECT_FALSE(m_preparer.prepare(path, settings(100000)).has_value());
}
