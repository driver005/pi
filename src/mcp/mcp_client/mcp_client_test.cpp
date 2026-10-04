#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.mcp.mcp_client;
import pi.testing.scripted_mcp_transport;

class McpClientTest : public testing::Test {
protected:
    McpClientTest() {
        McpClientOptions options;
        options.requestTimeoutMs = 2000;
        m_client = std::make_unique<McpClient>(options);
        auto transport = std::make_unique<ScriptedMcpTransport>();
        m_server = transport.get();
        m_server->answerInitialize(Json{{"tools", Json::object()}});
        m_transportOwner = std::move(transport);
    }

    Result<Json> connect() {
        return m_client->connect(std::move(m_transportOwner));
    }

    void waitUntil(const std::function<bool()>& condition) {
        for (int i = 0; i < 400 && !condition(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::unique_ptr<McpClient> m_client;
    ScriptedMcpTransport* m_server = nullptr;
    std::unique_ptr<IMcpTransport> m_transportOwner;
};

TEST_F(McpClientTest, HandshakeNegotiatesAndAnnouncesInitialized) {
    m_server->answerInitialize(Json{{"tools", Json{{"listChanged", true}}}}, "use the tools");
    const auto result = connect();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(m_client->connected());
    EXPECT_EQ(m_client->serverCapabilities()["tools"]["listChanged"], true);
    EXPECT_EQ(m_client->instructions(), "use the tools");
    EXPECT_EQ(m_client->serverInfo()["name"], "scripted");
    EXPECT_EQ(m_client->protocolVersion(), "2025-11-25");
    EXPECT_EQ(m_server->protocolVersion(), "2025-11-25");
    const auto init = m_server->sent("initialize");
    ASSERT_EQ(init.size(), 1U);
    EXPECT_EQ(init[0]["params"]["clientInfo"]["name"], "pi");
    EXPECT_EQ(init[0]["params"]["protocolVersion"], "2025-11-25");
    EXPECT_FALSE(init[0]["params"]["capabilities"].contains("roots"));
    EXPECT_EQ(m_server->sent("notifications/initialized").size(), 1U);
}

TEST_F(McpClientTest, UnsupportedProtocolVersionsAreRejected) {
    m_server->onRequest("initialize", [](const Json&) -> Result<Json> {
        return Json{{"protocolVersion", "1999-01-01"}, {"capabilities", Json::object()},
                    {"serverInfo", Json{{"name", "s"}, {"version", "1"}}}};
    });
    const auto result = connect();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("unsupported protocol version"), std::string::npos);
    EXPECT_FALSE(m_client->connected());
}

TEST_F(McpClientTest, StartAndInitializeFailuresCloseTheClient) {
    m_server->failStart("no such command");
    const auto result = connect();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "no such command");
}

TEST_F(McpClientTest, ListToolsFollowsCursors) {
    ASSERT_TRUE(connect().has_value());
    m_server->onRequest("tools/list", [](const Json& params) -> Result<Json> {
        if (!params.is_object() || !params.contains("cursor")) {
            return Json{{"tools", Json::array({Json{{"name", "a"}, {"inputSchema", Json::object()}}})}, {"nextCursor", "p2"}};
        }
        return Json{{"tools", Json::array({Json{{"name", "b"}, {"description", "second"}, {"inputSchema", Json::object()}}})}};
    });
    const auto tools = m_client->listTools({});
    ASSERT_TRUE(tools.has_value());
    ASSERT_EQ(tools->size(), 2U);
    EXPECT_EQ((*tools)[0].name, "a");
    EXPECT_EQ((*tools)[1].description, "second");
}

TEST_F(McpClientTest, DuplicateCursorsAreAnError) {
    ASSERT_TRUE(connect().has_value());
    m_server->onRequest("tools/list", [](const Json&) -> Result<Json> {
        return Json{{"tools", Json::array()}, {"nextCursor", "same"}};
    });
    const auto tools = m_client->listTools({});
    ASSERT_FALSE(tools.has_value());
    EXPECT_NE(tools.error().message.find("duplicate cursor"), std::string::npos);
}

TEST_F(McpClientTest, CallToolSendsArgumentsAndParsesTheResult) {
    ASSERT_TRUE(connect().has_value());
    m_server->onRequest("tools/call", [](const Json& params) -> Result<Json> {
        return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "echo " + params["arguments"]["x"].get<std::string>()}}})},
                    {"isError", false}};
    });
    const auto result = m_client->callTool("echo", Json{{"x", "hi"}}, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content[0]["text"], "echo hi");
    EXPECT_FALSE(result->isError);
    EXPECT_EQ(m_server->sent("tools/call")[0]["params"]["name"], "echo");
}

TEST_F(McpClientTest, JsonRpcErrorsKeepTheirCode) {
    ASSERT_TRUE(connect().has_value());
    const auto result = m_client->request("nope", Json(), {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "rpc:-32601");
}

TEST_F(McpClientTest, TimeoutsCancelTheRequest) {
    ASSERT_TRUE(connect().has_value());
    m_server->ignore("tools/call");
    McpRequestOptions options;
    options.timeoutMs = 50;
    const auto result = m_client->callTool("slow", Json(), options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "timeout");
    waitUntil([&] { return !m_server->sent("notifications/cancelled").empty(); });
    ASSERT_EQ(m_server->sent("notifications/cancelled").size(), 1U);
    EXPECT_EQ(m_server->sent("notifications/cancelled")[0]["params"]["reason"], "Request timed out");
}

TEST_F(McpClientTest, ProgressResetsTheTimeoutAndReachesTheCallback) {
    ASSERT_TRUE(connect().has_value());
    m_server->ignore("tools/call");
    std::vector<McpProgress> seen;
    std::mutex seenMutex;
    McpRequestOptions options;
    options.timeoutMs = 300;
    options.onProgress = [&](const McpProgress& progress) {
        const std::lock_guard<std::mutex> lock(seenMutex);
        seen.push_back(progress);
    };
    std::thread server([&] {
        waitUntil([&] { return !m_server->sent("tools/call").empty(); });
        const Json token = m_server->sent("tools/call")[0]["params"]["_meta"]["progressToken"];
        for (int step = 1; step <= 3; ++step) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            m_server->push(Json{{"jsonrpc", "2.0"}, {"method", "notifications/progress"},
                                {"params", Json{{"progressToken", token}, {"progress", step}, {"total", 3}, {"message", "step"}}}});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const Json id = m_server->sent("tools/call")[0]["id"];
        m_server->push(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json{{"content", Json::array()}}}});
    });
    const auto result = m_client->callTool("long", Json(), options);
    server.join();
    ASSERT_TRUE(result.has_value());
    const std::lock_guard<std::mutex> lock(seenMutex);
    ASSERT_EQ(seen.size(), 3U);
    EXPECT_EQ(seen[2].progress, 3);
    EXPECT_EQ(seen[2].total, 3);
    EXPECT_EQ(seen[0].message, "step");
}

TEST_F(McpClientTest, AbortingCancelsTheRequest) {
    ASSERT_TRUE(connect().has_value());
    m_server->ignore("tools/call");
    auto signal = std::make_shared<AbortSignal>();
    McpRequestOptions options;
    options.signal = signal;
    std::thread aborter([&] {
        waitUntil([&] { return !m_server->sent("tools/call").empty(); });
        signal->abort();
    });
    const auto result = m_client->callTool("slow", Json(), options);
    aborter.join();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "aborted");
    waitUntil([&] { return !m_server->sent("notifications/cancelled").empty(); });
    EXPECT_EQ(m_server->sent("notifications/cancelled").size(), 1U);
}

TEST_F(McpClientTest, AlreadyAbortedRequestsNeverSend) {
    ASSERT_TRUE(connect().has_value());
    auto signal = std::make_shared<AbortSignal>();
    signal->abort();
    McpRequestOptions options;
    options.signal = signal;
    EXPECT_EQ(m_client->request("tools/call", Json(), options).error().code, "aborted");
    EXPECT_TRUE(m_server->sent("tools/call").empty());
}

TEST_F(McpClientTest, ServerRequestsAreAnswered) {
    McpClientOptions options;
    options.roots = Json::array({Json{{"uri", "file:///work"}, {"name", "work"}}});
    m_client = std::make_unique<McpClient>(options);
    ASSERT_TRUE(connect().has_value());
    EXPECT_EQ(m_server->sent("initialize")[0]["params"]["capabilities"]["roots"], Json::object());
    m_server->push(Json{{"jsonrpc", "2.0"}, {"id", 41}, {"method", "ping"}});
    m_server->push(Json{{"jsonrpc", "2.0"}, {"id", 42}, {"method", "roots/list"}});
    m_server->push(Json{{"jsonrpc", "2.0"}, {"id", 43}, {"method", "sampling/createMessage"}});
    waitUntil([&] { return m_server->sent().size() >= 6; });
    std::map<std::int64_t, Json> replies;
    for (const auto& message : m_server->sent()) {
        if (message.contains("id") && !message.contains("method")) {
            replies[message["id"].get<std::int64_t>()] = message;
        }
    }
    ASSERT_EQ(replies.size(), 3U);
    EXPECT_EQ(replies[41]["result"], Json::object());
    EXPECT_EQ(replies[42]["result"]["roots"][0]["uri"], "file:///work");
    EXPECT_EQ(replies[43]["error"]["code"], -32601);
}

TEST_F(McpClientTest, NotificationListenersCanIssueRequests) {
    ASSERT_TRUE(connect().has_value());
    m_server->onRequest("tools/list", [](const Json&) -> Result<Json> {
        return Json{{"tools", Json::array({Json{{"name", "fresh"}, {"inputSchema", Json::object()}}})}};
    });
    std::atomic<int> tools{0};
    m_client->onNotification("notifications/tools/list_changed", [&](const Json&) {
        const auto listed = m_client->listTools({});
        tools = listed ? static_cast<int>(listed->size()) : -1;
    });
    m_server->push(Json{{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}});
    waitUntil([&] { return tools.load() != 0; });
    EXPECT_EQ(tools.load(), 1);
}

TEST_F(McpClientTest, TransportDropFailsPendingRequestsAndNotifiesListeners) {
    ASSERT_TRUE(connect().has_value());
    m_server->ignore("tools/call");
    std::atomic<bool> closed{false};
    m_client->onClose([&] { closed = true; });
    std::thread dropper([&] {
        waitUntil([&] { return !m_server->sent("tools/call").empty(); });
        m_server->drop();
    });
    const auto result = m_client->callTool("slow", Json(), {});
    dropper.join();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "closed");
    waitUntil([&] { return closed.load(); });
    EXPECT_TRUE(closed.load());
    EXPECT_FALSE(m_client->connected());
    EXPECT_FALSE(m_client->request("ping", Json(), {}).has_value());
}

TEST_F(McpClientTest, CloseIsIdempotentAndClosesTheTransport) {
    ASSERT_TRUE(connect().has_value());
    m_client->close();
    m_client->close();
    EXPECT_TRUE(m_server->closed());
    EXPECT_FALSE(m_client->connected());
}

TEST_F(McpClientTest, ResourcesAndTemplates) {
    ASSERT_TRUE(connect().has_value());
    m_server->onRequest("resources/list", [](const Json&) -> Result<Json> {
        return Json{{"resources", Json::array({Json{{"uri", "file:///a"}}})}};
    });
    m_server->onRequest("resources/read", [](const Json& params) -> Result<Json> {
        return Json{{"contents", Json::array({Json{{"uri", params["uri"]}, {"text", "body"}}})}};
    });
    const auto resources = m_client->listResources({});
    ASSERT_TRUE(resources.has_value());
    EXPECT_EQ((*resources)[0]["name"], "file:///a");
    const auto templates = m_client->listResourceTemplates({});
    ASSERT_TRUE(templates.has_value());
    EXPECT_TRUE(templates->empty());
    const auto read = m_client->readResource("file:///a", {});
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ((*read)["contents"][0]["text"], "body");
}

TEST_F(McpClientTest, ConnectOnlyOnce) {
    ASSERT_TRUE(connect().has_value());
    const auto second = m_client->connect(std::make_unique<ScriptedMcpTransport>());
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, "closed");
}
