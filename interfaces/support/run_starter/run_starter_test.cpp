#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.durable_session;
import pi.support.run_starter;

TEST(RunStarterTest, StartRunCreatesAConversationOwnedGenerationAndRecordsTheRun) {
    auto storage = std::make_shared<MemoryStorage>();
    DurableSession session(storage);
    RunStarter starter;
    std::int64_t conversationId = 0;
    Json live = Json::object();
    ASSERT_TRUE(session.commit([&](Transaction& tx) -> Result<void> {
        auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
        if (!created) {
            return std::unexpected(created.error());
        }
        conversationId = created->at("id").get<std::int64_t>();
        return starter.startRun(tx, conversationId, live, {11, 12});
    }).has_value());
    ASSERT_TRUE(live.contains("run"));
    EXPECT_EQ(live.at("run").at("inputs").dump(), "[11,12]");
    auto task = storage->task(live.at("run").at("taskId").get<std::int64_t>());
    ASSERT_TRUE(task.has_value() && task->has_value());
    EXPECT_EQ((*task)->at("kind"), "pi.generation");
    EXPECT_EQ((*task)->at("conversationId"), conversationId);
    EXPECT_FALSE((*task)->contains("owner"));
    EXPECT_EQ((*task)->at("background"), false);
    EXPECT_EQ((*task)->at("state").at("checkpoint").dump(), R"({"phase":"prepare","attempt":1})");
}

TEST(RunStarterTest, HandOverMovesRunControlOnlyFromTheOwner) {
    RunStarter starter;
    Json live = Json::object({{"run", Json::object({{"taskId", 5}, {"inputs", Json::array({1})}})}});
    starter.handOver(live, 9, 6);
    EXPECT_EQ(live.at("run").at("taskId"), 5);
    starter.handOver(live, 5, 6);
    EXPECT_EQ(live.at("run").at("taskId"), 6);
    EXPECT_EQ(live.at("run").at("inputs").size(), 1u);
    Json idle = Json::object();
    starter.handOver(idle, 5, 6);
    EXPECT_TRUE(idle.empty());
}
