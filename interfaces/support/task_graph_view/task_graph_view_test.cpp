#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.delta_applier;
import pi.support.json_equality;
import pi.support.task_graph_view;

class TaskGraphViewTest : public ::testing::Test {
protected:
    TaskGraphViewTest() : m_session(m_storage), m_graph(m_session) {}

    std::int64_t newConversation() {
        std::int64_t id = 0;
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
            if (!created) {
                return std::unexpected(created.error());
            }
            id = created->at("id").get<std::int64_t>();
            return {};
        }).has_value());
        return id;
    }

    std::int64_t newTask(std::int64_t conversationId) {
        std::int64_t id = 0;
        TaskOptions options;
        options.conversationId = conversationId;
        options.ownership = Json::object({{"kind", "conversation"}});
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createTask("test.task", 1, Json::object(), Json::object({{"phase", "start"}}), options);
            if (!created) {
                return std::unexpected(created.error());
            }
            id = *created;
            return {};
        }).has_value());
        return id;
    }

    Result<void> setState(std::int64_t id, const Json& state) {
        auto committed = m_session.commit([&](Transaction& tx) -> Result<void> {
            auto record = tx.task(id);
            if (!record) {
                return std::unexpected(record.error());
            }
            Json next = **record;
            next["state"] = state;
            return tx.setTask(next);
        });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        return {};
    }

    std::shared_ptr<MemoryStorage> m_storage = std::make_shared<MemoryStorage>();
    DurableSession m_session;
    TaskGraphView m_graph;
};

TEST_F(TaskGraphViewTest, ShowsLiveTasksAndDropsTerminalOnes) {
    const std::int64_t conversation = newConversation();
    auto graph = m_graph.state();
    ASSERT_TRUE(graph.has_value()) << graph.error().message;
    EXPECT_TRUE((*graph)->snapshot().value.at("tasks").empty());
    Json replay = (*graph)->snapshot().value;
    (*graph)->subscribe([&](const Json& ops, std::int64_t, const ServiceContext&) {
        auto applied = DeltaApplier().apply(replay, ops);
        ASSERT_TRUE(applied.has_value());
        replay = *applied;
    });
    const std::int64_t task = newTask(conversation);
    const std::string key = std::to_string(task);
    Json node = (*graph)->snapshot().value.at("tasks").at(key);
    EXPECT_EQ(node.at("kind"), "test.task");
    EXPECT_EQ(node.at("conversationId"), conversation);
    EXPECT_EQ(node.at("state").at("status"), "pending");
    EXPECT_EQ(node.at("state").at("phase"), "start");
    EXPECT_TRUE(node.at("conversations").empty());
    ASSERT_TRUE(setState(task, Json::object({{"status", "running"}, {"checkpoint", Json::object({{"phase", "go"}})}})).has_value());
    node = (*graph)->snapshot().value.at("tasks").at(key);
    EXPECT_EQ(node.at("state").at("status"), "running");
    EXPECT_EQ(node.at("state").at("phase"), "go");
    ASSERT_TRUE(setState(task, Json::object({{"status", "completing"}, {"outcome", Json::object({{"status", "done"}})}})).has_value());
    EXPECT_EQ((*graph)->snapshot().value.at("tasks").at(key).at("state"), Json::object({{"status", "completing"}, {"outcome", "done"}}));
    EXPECT_TRUE(JsonEquality().equal(replay, (*graph)->snapshot().value));
}

TEST_F(TaskGraphViewTest, BuildsFromStorageWhenNoMountIsHeld) {
    const std::int64_t conversation = newConversation();
    const std::int64_t first = newTask(conversation);
    const std::int64_t second = newTask(conversation);
    auto graph = m_graph.state();
    ASSERT_TRUE(graph.has_value());
    const Json tasks = (*graph)->snapshot().value.at("tasks");
    EXPECT_EQ(tasks.size(), 2u);
    EXPECT_TRUE(tasks.contains(std::to_string(first)));
    EXPECT_TRUE(tasks.contains(std::to_string(second)));
    auto again = m_graph.state();
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->get(), graph->get());
}

TEST_F(TaskGraphViewTest, ClosedSessionsRefuseMounts) {
    ASSERT_TRUE(m_session.close().has_value());
    EXPECT_FALSE(m_graph.state().has_value());
}
