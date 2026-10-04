#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.settings_merger;

class SettingsMergerTest : public testing::Test {
protected:
    Json parse(const char* text) { return Json::parse(text); }

    SettingsMerger m_merger;
};

TEST_F(SettingsMergerTest, ProjectWinsAndObjectsMergeRecursively) {
    const Json merged = m_merger.merge(parse(R"({"a":1,"retry":{"enabled":true,"maxRetries":5},"list":[1]})"),
                                       parse(R"({"a":2,"retry":{"maxRetries":1},"list":[2]})"));
    EXPECT_EQ(merged["a"], 2);
    EXPECT_EQ(merged["retry"]["enabled"], true);
    EXPECT_EQ(merged["retry"]["maxRetries"], 1);
    EXPECT_EQ(merged["list"], parse("[2]"));
}

TEST_F(SettingsMergerTest, DefaultToolsModifiersAppendPlainListsReplace) {
    EXPECT_EQ(m_merger.merge(parse(R"({"defaultTools":["read"]})"), parse(R"({"defaultTools":["+grep","-read"]})"))["defaultTools"],
              parse(R"(["read","+grep","-read"])"));
    EXPECT_EQ(m_merger.merge(parse(R"({"defaultTools":["read"]})"), parse(R"({"defaultTools":["bash"]})"))["defaultTools"],
              parse(R"(["bash"])"));
    EXPECT_EQ(m_merger.merge(parse("{}"), parse(R"({"defaultTools":["+grep"]})"))["defaultTools"], parse(R"(["+grep"])"));
}

TEST_F(SettingsMergerTest, ResolveDefaultTools) {
    EXPECT_EQ(m_merger.resolveDefaultTools({"+grep"}), (std::vector<std::string>{"read", "bash", "edit", "write", "grep"}));
    EXPECT_EQ(m_merger.resolveDefaultTools({"-bash", "-write"}), (std::vector<std::string>{"read", "edit"}));
    EXPECT_EQ(m_merger.resolveDefaultTools({"read", "ls", "+grep"}), (std::vector<std::string>{"read", "ls", "grep"}));
    EXPECT_TRUE(m_merger.resolveDefaultTools({}).empty());
}

TEST_F(SettingsMergerTest, MigratesLegacyFields) {
    const Json migrated = m_merger.migrate(parse(R"({"queueMode":"all","websockets":true,
        "skills":{"enableSkillCommands":false,"customDirectories":["/s"]},
        "retry":{"maxDelayMs":9000,"enabled":true}})"));
    EXPECT_EQ(migrated["steeringMode"], "all");
    EXPECT_FALSE(migrated.contains("queueMode"));
    EXPECT_EQ(migrated["transport"], "websocket");
    EXPECT_EQ(migrated["skills"], parse(R"(["/s"])"));
    EXPECT_EQ(migrated["enableSkillCommands"], false);
    EXPECT_EQ(migrated["retry"]["provider"]["maxRetryDelayMs"], 9000);
    EXPECT_FALSE(migrated["retry"].contains("maxDelayMs"));
}

TEST_F(SettingsMergerTest, MigrationKeepsExplicitNewFields) {
    const Json migrated = m_merger.migrate(parse(R"({"queueMode":"all","steeringMode":"one-at-a-time","transport":"sse","websockets":true,
        "skills":{"customDirectories":[]},"retry":{"maxDelayMs":1,"provider":{"maxRetryDelayMs":2}}})"));
    EXPECT_EQ(migrated["steeringMode"], "one-at-a-time");
    EXPECT_EQ(migrated["transport"], "sse");
    EXPECT_FALSE(migrated.contains("skills"));
    EXPECT_EQ(migrated["retry"]["provider"]["maxRetryDelayMs"], 2);
}
