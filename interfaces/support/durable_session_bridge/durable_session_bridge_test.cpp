#include <gtest/gtest.h>

import std;
import pi.support.durable_session_bridge;
import pi.testing.durable_harness_fixture;

class DurableSessionBridgeTest : public ::testing::Test {
protected:
    DurableSessionBridgeTest() {
        EXPECT_TRUE(m_fixture.open().has_value());
        m_bridge = std::make_unique<DurableSessionBridge>(m_fixture.root(), "/work");
    }

    Json call(const std::string& method, const Json& params = Json::object()) {
        return m_bridge->call(method, params, nullptr);
    }

    DurableHarnessFixture m_fixture;
    std::unique_ptr<DurableSessionBridge> m_bridge;
};

TEST_F(DurableSessionBridgeTest, AppendsPluginEntriesAndReadsThemBackOldestFirst) {
    const Json first = call("appendEntry", Json{{"customType", "note"}, {"data", Json{{"n", 1}}}});
    ASSERT_TRUE(first.contains("id")) << first.dump();
    ASSERT_TRUE(call("appendEntry", Json{{"customType", "note"}, {"data", Json{{"n", 2}}}}).contains("id"));
    const Json entries = call("sessionManager.getEntries");
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0]["kind"], "pi.plugin-entry");
    EXPECT_EQ(entries[0]["data"]["data"]["n"], 1);
    EXPECT_EQ(entries[1]["data"]["data"]["n"], 2);
}

TEST_F(DurableSessionBridgeTest, ReportsTheConversationAndDirectory) {
    EXPECT_EQ(call("sessionManager.getCwd")["cwd"], "/work");
    EXPECT_EQ(call("sessionManager.getSessionId")["id"], std::to_string(m_fixture.root()->id()));
    EXPECT_EQ(call("isIdle")["idle"], true);
    EXPECT_EQ(call("waitForIdle")["ok"], true);
}

TEST_F(DurableSessionBridgeTest, SendsUserMessagesAsInput) {
    m_fixture.faux().enqueue(m_fixture.faux().textResponse("pong"));
    EXPECT_EQ(call("sendUserMessage", Json{{"text", "ping"}})["ok"], true);
    EXPECT_EQ(call("waitForIdle")["ok"], true);
    EXPECT_EQ(call("isIdle")["idle"], true);
    bool answered = false;
    for (const Json& entry : call("sessionManager.getEntries")) {
        answered = answered || entry.value("kind", std::string()) == "pi.assistant";
    }
    EXPECT_TRUE(answered);
}

TEST_F(DurableSessionBridgeTest, RejectsMalformedAndUnsupportedCalls) {
    EXPECT_TRUE(call("appendEntry", Json::object()).contains("error"));
    EXPECT_TRUE(call("sendUserMessage", Json::object()).contains("error"));
    EXPECT_TRUE(call("setModel", Json{{"provider", "faux"}, {"id", "m"}}).contains("error"));
    EXPECT_TRUE(call("newSession").contains("error"));
}
