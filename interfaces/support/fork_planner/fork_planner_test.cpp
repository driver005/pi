#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.fork_planner;

class ForkPlannerTest : public ::testing::Test {
protected:
    Json conversationScope(std::int64_t id) {
        return Json::object({{"kind", "conversation"}, {"conversationId", id}});
    }

    Json documentWrite(std::int64_t id, const std::string& kind, std::int64_t conversationId, const std::string& fork,
                       const std::string& key = "") {
        Json record = Json::object({{"id", id}, {"kind", kind}, {"scope", conversationScope(conversationId)}});
        if (!key.empty()) {
            record["key"] = key;
        }
        record["history"] = "latest";
        record["fork"] = fork;
        return Json::object({{"type", "document.create"},
                             {"record", record},
                             {"content", Json::object({{"kind", "base"}, {"version", 1}, {"value", Json::object()}})}});
    }

    void seed() {
        ASSERT_TRUE(m_storage.commit({Json::object({{"type", "conversation"}, {"value", Json::object({{"id", 1}})}})}));
        ASSERT_TRUE(m_storage.commit({Json::object({{"type", "entry"},
                                                    {"value", Json::object({{"id", 2}, {"conversationId", 1}, {"kind", "message"}})}})}));
        ASSERT_TRUE(m_storage.commit({documentWrite(3, "a", 1, "current"), documentWrite(4, "b", 1, "asOf")}));
    }

    MemoryStorage m_storage;
    ForkPlanner m_planner;
};

TEST_F(ForkPlannerTest, CopiesCurrentPolicyDocumentsAndOmitsOthers) {
    seed();
    auto copies = m_planner.prepare(m_storage, 1, 2, 100);
    ASSERT_TRUE(copies.has_value());
    // "b" is asOf: it did not exist at the entry's commit, so only the current-policy document "a" copies.
    ASSERT_EQ(copies->size(), 1u);
    EXPECT_EQ((*copies)[0].record.at("kind"), "a");
    EXPECT_EQ((*copies)[0].record.at("scope").at("conversationId"), 100);
    EXPECT_EQ((*copies)[0].source.at("id"), 3);
    EXPECT_EQ((*copies)[0].source.at("at"), "current");
}

TEST_F(ForkPlannerTest, UnknownEntryFails) {
    seed();
    auto copies = m_planner.prepare(m_storage, 1, 999, 100);
    ASSERT_FALSE(copies.has_value());
    EXPECT_NE(copies.error().message.find("is not visible"), std::string::npos);
}
