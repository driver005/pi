#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.session_data_codec;

class SessionDataCodecTest : public testing::Test {
protected:
    SessionDataCodec m_codec;
};

TEST_F(SessionDataCodecTest, BashResultOmitsAbsentFields) {
    BashResult result;
    result.output = "hi\n";
    result.exitCode = 0;
    EXPECT_EQ(m_codec.bash(result), (Json{{"output", "hi\n"}, {"exitCode", 0}, {"cancelled", false}, {"truncated", false}}));
    result.exitCode.reset();
    result.cancelled = true;
    result.fullOutputPath = "/tmp/x";
    const Json cancelled = m_codec.bash(result);
    EXPECT_FALSE(cancelled.contains("exitCode"));
    EXPECT_EQ(cancelled["fullOutputPath"], "/tmp/x");
}

TEST_F(SessionDataCodecTest, CompactionResult) {
    CompactionResult result;
    result.summary = "s";
    result.firstKeptEntryId = "e1";
    result.tokensBefore = 100;
    result.estimatedTokensAfter = 20;
    result.details = Json{{"readFiles", Json::array()}};
    const Json json = m_codec.compaction(result);
    EXPECT_EQ(json["firstKeptEntryId"], "e1");
    EXPECT_EQ(json["estimatedTokensAfter"], 20);
    EXPECT_FALSE(json.contains("usage"));
    EXPECT_TRUE(json.contains("details"));
}

TEST_F(SessionDataCodecTest, StatsWithAndWithoutContextUsage) {
    SessionStats stats;
    stats.sessionId = "abc";
    stats.userMessages = 2;
    stats.inputTokens = 5;
    stats.totalTokens = 5;
    stats.cost = 0.5;
    Json json = m_codec.stats(stats);
    EXPECT_EQ(json["tokens"]["input"], 5);
    EXPECT_FALSE(json.contains("contextUsage"));
    ContextUsage usage;
    usage.contextWindow = 1000;
    stats.contextUsage = usage;
    json = m_codec.stats(stats);
    EXPECT_TRUE(json["contextUsage"]["tokens"].is_null());
    EXPECT_EQ(json["contextUsage"]["contextWindow"], 1000);
}

TEST_F(SessionDataCodecTest, TreeQueueForkableAndCommands) {
    SessionTreeNode child;
    child.entry.body = Json{{"id", "c"}};
    SessionTreeNode root;
    root.entry.body = Json{{"id", "r"}};
    root.label = "mark";
    root.children.push_back(child);
    const Json tree = m_codec.tree({root});
    EXPECT_EQ(tree[0]["entry"]["id"], "r");
    EXPECT_EQ(tree[0]["label"], "mark");
    EXPECT_EQ(tree[0]["children"][0]["entry"]["id"], "c");

    QueuedInput queued;
    queued.followUp = {"later"};
    EXPECT_EQ(m_codec.queued(queued), (Json{{"steering", Json::array()}, {"followUp", {"later"}}}));
    EXPECT_EQ(m_codec.forkable({ForkableMessage{"e", "t"}}), (Json::array({Json{{"entryId", "e"}, {"text", "t"}}})));

    SlashCommandInfo command;
    command.name = "skill:pdf";
    command.source = "skill";
    command.sourceInfo.path = "/s";
    const Json commands = m_codec.slashCommands({command});
    EXPECT_EQ(commands[0]["sourceInfo"]["path"], "/s");
    EXPECT_FALSE(commands[0].contains("description"));
}

TEST_F(SessionDataCodecTest, ToolResultAndModelCycle) {
    AgentToolResult result;
    result.content.push_back(TextContent{"data", std::nullopt});
    result.terminate = true;
    const Json json = m_codec.toolResult(result);
    EXPECT_EQ(json["content"][0]["type"], "text");
    EXPECT_TRUE(json["details"].is_object());
    EXPECT_EQ(json["terminate"], true);
    ModelCycleResult cycle;
    cycle.model.id = "m";
    cycle.thinkingLevel = ThinkingLevel::High;
    cycle.isScoped = true;
    const Json wire = m_codec.modelCycle(cycle);
    EXPECT_EQ(wire["model"]["id"], "m");
    EXPECT_EQ(wire["thinkingLevel"], "high");
}
