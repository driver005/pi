#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.session_migrator;

class SessionMigratorTest : public testing::Test {
protected:
    std::vector<Json> entries(std::initializer_list<const char*> texts) {
        std::vector<Json> out;
        for (const char* text : texts) {
            out.push_back(Json::parse(text));
        }
        return out;
    }

    SessionMigrator m_migrator;
};

TEST_F(SessionMigratorTest, CurrentVersionIsUntouched) {
    auto list = entries({R"({"type":"session","version":3,"id":"s"})", R"({"type":"message","id":"a","parentId":null})"});
    EXPECT_FALSE(m_migrator.migrate(list));
    EXPECT_EQ(list[0]["version"], 3);
}

TEST_F(SessionMigratorTest, V1GainsIdsParentsAndCompactionIds) {
    auto list = entries({R"({"type":"session","id":"s"})",
                         R"({"type":"message","message":{"role":"user","content":"a"}})",
                         R"({"type":"message","message":{"role":"assistant","content":[]}})",
                         R"({"type":"compaction","summary":"s","firstKeptEntryIndex":2,"tokensBefore":5})"});
    EXPECT_TRUE(m_migrator.migrate(list));
    EXPECT_EQ(list[0]["version"], 3);
    EXPECT_TRUE(list[1]["parentId"].is_null());
    EXPECT_EQ(list[2]["parentId"], list[1]["id"]);
    EXPECT_EQ(list[3]["parentId"], list[2]["id"]);
    EXPECT_EQ(list[3]["firstKeptEntryId"], list[2]["id"]);
    EXPECT_FALSE(list[3].contains("firstKeptEntryIndex"));
}

TEST_F(SessionMigratorTest, V2RenamesHookMessageRole) {
    auto list = entries({R"({"type":"session","version":2,"id":"s"})",
                         R"({"type":"message","id":"a","parentId":null,"message":{"role":"hookMessage","customType":"x"}})"});
    EXPECT_TRUE(m_migrator.migrate(list));
    EXPECT_EQ(list[0]["version"], 3);
    EXPECT_EQ(list[1]["message"]["role"], "custom");
    EXPECT_EQ(list[1]["id"], "a");
}
