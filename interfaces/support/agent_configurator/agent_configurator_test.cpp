#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.agent_configurator;
import pi.support.durable_session;

class AgentConfiguratorTest : public ::testing::Test {
protected:
    AgentConfiguratorTest() : m_session(m_storage) {}

    std::int64_t newConversation(const Json& ownership = Json::object({{"kind", "ownerless"}})) {
        std::int64_t id = 0;
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(ownership);
            if (!created) {
                return std::unexpected(created.error());
            }
            id = created->at("id").get<std::int64_t>();
            return m_configurator.createAgent(tx, *created);
        }).has_value());
        return id;
    }

    Json agent(std::int64_t conversationId) {
        DocAddressArgs args;
        args.owner = conversationId;
        auto snapshot = m_session.snapshot(m_documents.agent(), args);
        return snapshot && *snapshot ? **snapshot : Json(nullptr);
    }

    void configure(std::int64_t conversationId, const Json& change) {
        ASSERT_TRUE(m_session.commit([&](Transaction& tx) { return m_configurator.configure(tx, conversationId, change); }).has_value());
    }

    std::shared_ptr<MemoryStorage> m_storage = std::make_shared<MemoryStorage>();
    DurableSession m_session;
    AgentConfigurator m_configurator;
    BuiltinDocuments m_documents;
};

TEST_F(AgentConfiguratorTest, GivenFieldsReplaceNullClearsAbsentChangesNothing) {
    const std::int64_t id = newConversation();
    EXPECT_EQ(agent(id), Json::object());
    configure(id, Json::parse(R"({"model":{"provider":"p","modelId":"m"},"thinkingLevel":"high","instructions":"be brief","cwd":"/w"})"));
    EXPECT_EQ(agent(id).at("model").at("modelId"), "m");
    configure(id, Json::parse(R"({"thinkingLevel":null,"extensions":{"add":["x"]}})"));
    Json state = agent(id);
    EXPECT_FALSE(state.contains("thinkingLevel"));
    EXPECT_EQ(state.at("instructions"), "be brief");
    EXPECT_EQ(state.at("extensions").at("add")[0], "x");
    configure(id, Json::object());
    EXPECT_EQ(agent(id), state);
}

TEST_F(AgentConfiguratorTest, AddToolsAppendsToListsAndShrinksRemovals) {
    const std::int64_t id = newConversation();
    ASSERT_TRUE(m_session.commit([&](Transaction& tx) { return m_configurator.addTools(tx, id, {"a"}); }).has_value());
    EXPECT_FALSE(agent(id).contains("tools"));
    configure(id, Json::parse(R"({"tools":["x"]})"));
    ASSERT_TRUE(m_session.commit([&](Transaction& tx) { return m_configurator.addTools(tx, id, {"x", "y"}); }).has_value());
    EXPECT_EQ(agent(id).at("tools").dump(), R"(["x","y"])");
    configure(id, Json::parse(R"({"tools":{"remove":["a","b","c"]}})"));
    ASSERT_TRUE(m_session.commit([&](Transaction& tx) { return m_configurator.addTools(tx, id, {"b"}); }).has_value());
    EXPECT_EQ(agent(id).at("tools").dump(), R"({"remove":["a","c"]})");
}
