#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.mcp.mcp_server_connection;
import pi.testing.recording_sleeper;
import pi.testing.scripted_mcp_connector;

class McpServerConnectionTest : public testing::Test {
protected:
    McpServerConfig config(bool http = false) {
        McpServerConfig out;
        out.name = "docs";
        out.http = http;
        out.command = "docs-server";
        out.url = "https://example.com/mcp";
        return out;
    }

    void build(McpServerConfig serverConfig) {
        m_connection = std::make_unique<McpServerConnection>(std::move(serverConfig), "/work/my project", "9.9",
                                                             m_connector, m_sleeper);
        m_connection->setToolsListener([this](McpServerConnection&) { m_toolEvents.fetch_add(1); });
        m_connection->setChangeListener([this](McpServerConnection& changed) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_states.push_back(changed.state());
        });
    }

    Json toolList(const std::vector<std::string>& names) {
        Json tools = Json::array();
        for (const std::string& name : names) {
            tools.push_back(Json{{"name", name}, {"inputSchema", Json{{"type", "object"}}}});
        }
        return Json{{"tools", tools}};
    }

    ScriptedMcpConnector::Setup server(std::vector<std::string> names, const std::string& reply = "ok") {
        return [this, names, reply](ScriptedMcpTransport& transport) {
            transport.answerInitialize(Json{{"tools", Json{{"listChanged", true}}}}, "be nice");
            transport.onRequest("tools/list", [this, names](const Json&) -> Result<Json> {
                const std::lock_guard<std::mutex> lock(m_mutex);
                return toolList(m_listed.empty() ? names : m_listed);
            });
            transport.onRequest("tools/call", [reply](const Json&) -> Result<Json> {
                return Json{{"content", Json::array({Json{{"type", "text"}, {"text", reply}}})}};
            });
        };
    }

    bool waitUntil(const std::function<bool()>& condition) {
        for (int i = 0; i < 600 && !condition(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return condition();
    }

    std::vector<McpServerState> states() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_states;
    }

    // The connection's listeners use the members above it, so it is declared (and destroyed) last.
    std::mutex m_mutex;
    std::vector<McpServerState> m_states;
    std::vector<std::string> m_listed;
    std::atomic<int> m_toolEvents{0};
    ScriptedMcpConnector m_connector;
    RecordingSleeper m_sleeper;
    std::unique_ptr<McpServerConnection> m_connection;
};

TEST_F(McpServerConnectionTest, ConnectListsToolsAndReportsState) {
    m_connector.enqueue(server({"search", "fetch"}));
    build(config());
    ASSERT_TRUE(m_connection->connect().has_value());
    EXPECT_EQ(m_connection->state(), McpServerState::Connected);
    ASSERT_EQ(m_connection->tools().size(), 2U);
    EXPECT_EQ(m_connection->tools()[0].name, "search");
    EXPECT_EQ(*m_connection->instructions(), "be nice");
    EXPECT_EQ(m_toolEvents.load(), 1);
    EXPECT_EQ(states().front(), McpServerState::Connecting);
    EXPECT_EQ(states().back(), McpServerState::Connected);
    const McpClientOptions options = m_connector.options().at(0);
    EXPECT_EQ(options.version, "9.9");
    EXPECT_EQ(options.requestTimeoutMs, 60000);
    EXPECT_EQ(options.roots[0]["uri"], "file:///work/my%20project");
    EXPECT_EQ(options.roots[0]["name"], "my project");
    EXPECT_EQ(m_connector.directories().at(0), "/work/my project");
}

TEST_F(McpServerConnectionTest, TimeoutComesFromTheConfig) {
    McpServerConfig timed = config();
    timed.timeoutSeconds = 2.5;
    m_connector.enqueue(server({}));
    build(timed);
    EXPECT_EQ(m_connection->timeoutMs(), 2500);
    ASSERT_TRUE(m_connection->connect().has_value());
    EXPECT_EQ(m_connector.options().at(0).requestTimeoutMs, 2500);
}

TEST_F(McpServerConnectionTest, ConnectIsIdempotentAndShared) {
    m_connector.enqueue(server({"a"}));
    build(config());
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([this]() { EXPECT_TRUE(m_connection->connect().has_value()); });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_TRUE(m_connection->connect().has_value());
    EXPECT_EQ(m_connector.attempts(), 1);
}

TEST_F(McpServerConnectionTest, CallToolConnectsLazily) {
    m_connector.enqueue(server({"a"}, "fine"));
    build(config());
    const auto result = m_connection->callTool("a", Json{{"x", 1}}, McpRequestOptions{});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content[0]["text"], "fine");
    EXPECT_EQ(m_connection->state(), McpServerState::Connected);
}

TEST_F(McpServerConnectionTest, ServersWithoutToolsCapabilitySkipToolsList) {
    m_connector.enqueue([](ScriptedMcpTransport& transport) { transport.answerInitialize(Json::object()); });
    build(config());
    ASSERT_TRUE(m_connection->connect().has_value());
    EXPECT_TRUE(m_connection->tools().empty());
    EXPECT_TRUE(m_connector.transports().at(0)->sent("tools/list").empty());
}

TEST_F(McpServerConnectionTest, FailedConnectsAreReported) {
    m_connector.enqueueFailure(Error{"protocol", "bad handshake"});
    build(config());
    const auto result = m_connection->connect();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "protocol");
    EXPECT_EQ(result.error().message, "MCP server \"docs\" failed to connect: bad handshake");
    EXPECT_EQ(m_connection->state(), McpServerState::Failed);
    EXPECT_EQ(m_connection->error(), "bad handshake");
    EXPECT_TRUE(m_sleeper.delays().empty());
}

TEST_F(McpServerConnectionTest, TransientHttpFailuresAreRetried) {
    m_connector.enqueueFailure(Error{"http:503", "busy"});
    m_connector.enqueueFailure(Error{"transport", "reset"});
    m_connector.enqueue(server({"a"}));
    build(config(true));
    ASSERT_TRUE(m_connection->connect().has_value());
    EXPECT_EQ(m_connector.attempts(), 3);
    EXPECT_EQ(m_sleeper.delays(), (std::vector<std::int64_t>{250, 1000}));
}

TEST_F(McpServerConnectionTest, RetriesStopAfterTwoAndSkipPermanentErrors) {
    for (int i = 0; i < 4; ++i) {
        m_connector.enqueueFailure(Error{"http:500", "down"});
    }
    build(config(true));
    EXPECT_FALSE(m_connection->connect().has_value());
    EXPECT_EQ(m_connector.attempts(), 3);

    ScriptedMcpConnector other;
    other.enqueueFailure(Error{"http:404", "gone"});
    McpServerConnection permanent(config(true), "/w", "1", other, m_sleeper);
    EXPECT_FALSE(permanent.connect().has_value());
    EXPECT_EQ(other.attempts(), 1);

    ScriptedMcpConnector stdio;
    stdio.enqueueFailure(Error{"transport", "spawn failed"});
    McpServerConnection local(config(false), "/w", "1", stdio, m_sleeper);
    EXPECT_FALSE(local.connect().has_value());
    EXPECT_EQ(stdio.attempts(), 1);
}

TEST_F(McpServerConnectionTest, AuthRequiredMarksTheServer) {
    m_connector.enqueueFailure(Error{"auth_required", "401"});
    build(config(true));
    const auto result = m_connection->connect();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "auth_required");
    EXPECT_NE(result.error().message.find("requires authentication"), std::string::npos);
    EXPECT_EQ(m_connection->state(), McpServerState::NeedsAuth);
    EXPECT_EQ(m_connection->error(), "");
}

TEST_F(McpServerConnectionTest, ExpiredSessionsRetryOnceOnANewOne) {
    m_connector.enqueue([this](ScriptedMcpTransport& transport) {
        server({"a"})(transport);
        transport.failSend("tools/call", Error{"session_expired", "MCP session expired"});
    });
    m_connector.enqueue(server({"a"}, "second"));
    build(config(true));
    const auto result = m_connection->callTool("a", Json::object(), McpRequestOptions{});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content[0]["text"], "second");
    EXPECT_EQ(m_connector.attempts(), 2);
}

TEST_F(McpServerConnectionTest, AuthErrorsDuringACallNeedSignIn) {
    m_connector.enqueue([this](ScriptedMcpTransport& transport) {
        server({"a"})(transport);
        transport.failSend("tools/call", Error{"auth_required", "401"});
    });
    build(config(true));
    const auto result = m_connection->callTool("a", Json::object(), McpRequestOptions{});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "auth_required");
    EXPECT_EQ(m_connection->state(), McpServerState::NeedsAuth);
}

TEST_F(McpServerConnectionTest, DroppedConnectionsReconnectOnTheNextCall) {
    m_connector.enqueue(server({"a"}, "first"));
    m_connector.enqueue(server({"a"}, "again"));
    build(config());
    ASSERT_TRUE(m_connection->connect().has_value());
    m_connector.transports().at(0)->drop();
    ASSERT_TRUE(waitUntil([&]() { return m_connection->state() == McpServerState::Disconnected; }));
    EXPECT_EQ(m_connection->error(), "Connection closed");
    const auto result = m_connection->callTool("a", Json::object(), McpRequestOptions{});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content[0]["text"], "again");
    EXPECT_EQ(m_connection->state(), McpServerState::Connected);
}

TEST_F(McpServerConnectionTest, ListChangedRefreshesTheTools) {
    m_connector.enqueue(server({"a"}));
    build(config());
    ASSERT_TRUE(m_connection->connect().has_value());
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_listed = {"a", "b"};
    }
    m_connector.transports().at(0)->push(Json{{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}});
    ASSERT_TRUE(waitUntil([&]() { return m_connection->tools().size() == 2; }));
    EXPECT_EQ(m_toolEvents.load(), 2);
}

TEST_F(McpServerConnectionTest, CloseShutsTheServerDown) {
    m_connector.enqueue(server({"a"}));
    build(config());
    ASSERT_TRUE(m_connection->connect().has_value());
    m_connection->close();
    m_connection->close();
    EXPECT_EQ(m_connection->state(), McpServerState::Closed);
    const auto result = m_connection->callTool("a", Json::object(), McpRequestOptions{});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "closed");
}
