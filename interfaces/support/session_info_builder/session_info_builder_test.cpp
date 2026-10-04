#include <gtest/gtest.h>

import std;
import pi.support.session_info_builder;

class SessionInfoBuilderTest : public testing::Test {
protected:
    std::string header() {
        return R"({"type":"session","version":3,"id":"s1","timestamp":"2025-01-01T00:00:00.000Z","cwd":"/w","parentSession":"/p.jsonl"})" "\n";
    }

    SessionInfoBuilder m_builder;
};

TEST_F(SessionInfoBuilderTest, SummarizesMessagesNameAndActivity) {
    const std::string content = header() +
        R"({"type":"message","id":"a","parentId":null,"timestamp":"2025-01-01T00:00:01.000Z","message":{"role":"user","content":"hello world","timestamp":1735689601000}})" "\n"
        R"({"type":"message","id":"b","parentId":"a","timestamp":"2025-01-01T00:00:02.000Z","message":{"role":"assistant","content":[{"type":"text","text":"hi"},{"type":"thinking","thinking":"x"},{"type":"text","text":"there"}],"timestamp":1735689602000}})" "\n"
        R"({"type":"message","id":"c","parentId":"b","timestamp":"2025-01-01T00:00:03.000Z","message":{"role":"toolResult","content":[],"toolCallId":"t","toolName":"x"}})" "\n"
        R"({"type":"session_info","id":"d","parentId":"c","timestamp":"2025-01-01T00:00:04.000Z","name":" My Chat "})" "\n";
    const auto info = m_builder.build("/s/a.jsonl", content, 5);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->id, "s1");
    EXPECT_EQ(info->cwd, "/w");
    EXPECT_EQ(info->parentSessionPath, "/p.jsonl");
    EXPECT_EQ(info->name, "My Chat");
    EXPECT_EQ(info->messageCount, 3);
    EXPECT_EQ(info->firstMessage, "hello world");
    EXPECT_EQ(info->allMessagesText, "hello world hi there");
    EXPECT_EQ(info->modifiedMs, 1735689602000LL);
    EXPECT_EQ(info->createdMs, 1735689600000LL);
}

TEST_F(SessionInfoBuilderTest, EmptySessionUsesHeaderTimeAndPlaceholder) {
    const auto info = m_builder.build("/s/a.jsonl", header(), 5);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->firstMessage, "(no messages)");
    EXPECT_EQ(info->modifiedMs, 1735689600000LL);
    EXPECT_EQ(info->messageCount, 0);
}

TEST_F(SessionInfoBuilderTest, ClearedNameAndNonHeader) {
    const std::string content = header() +
        R"({"type":"session_info","id":"d","parentId":null,"timestamp":"t","name":"x"})" "\n"
        R"({"type":"session_info","id":"e","parentId":"d","timestamp":"t","name":""})" "\n";
    EXPECT_FALSE(m_builder.build("p", content, 0)->name.has_value());
    EXPECT_FALSE(m_builder.build("p", R"({"type":"message"})" "\n", 0).has_value());
    EXPECT_FALSE(m_builder.build("p", "", 0).has_value());
}
