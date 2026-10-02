#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.mcp.mcp_client;
import pi.mcp.streamable_http_mcp_transport;
import pi.support.header_merger;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class StreamableHttpMcpTransportTest : public testing::Test {
protected:
    StreamableHttpMcpTransportTest() {
        m_options.url = "http://mcp.test/mcp";
        m_options.openGetStream = false;
        m_options.reconnectMaxRetries = 2;
    }

    void build(bool start = true) {
        m_transport = std::make_unique<StreamableHttpMcpTransport>(m_http, m_sleeper, m_options);
        m_transport->setMessageListener([this](const Json& message) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_messages.push_back(message);
        });
        m_transport->setErrorListener([this](const Error& error) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_errors.push_back(error);
        });
        m_transport->setCloseListener([this]() { m_closes.fetch_add(1); });
        if (start) {
            ASSERT_TRUE(m_transport->start().has_value());
        }
    }

    HttpResponse reply(int status, const std::string& type, const std::string& body,
                       HttpHeaders extra = {}) {
        HttpResponse response;
        response.status = status;
        response.body = body;
        if (!type.empty()) {
            response.headers.emplace_back("Content-Type", type);
        }
        for (auto& header : extra) {
            response.headers.push_back(std::move(header));
        }
        return response;
    }

    std::string header(const HttpRequest& request, const std::string& name) {
        return HeaderMerger().find(request.headers, name).value_or("");
    }

    Json rpcResult(int id) {
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json{{"ok", true}}}};
    }

    Json request(int id, const std::string& method = "tools/list") {
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    }

    bool waitUntil(const std::function<bool()>& condition) {
        for (int i = 0; i < 600 && !condition(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return condition();
    }

    std::vector<Json> messages() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_messages;
    }

    std::vector<Error> errors() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_errors;
    }

    McpHttpOptions m_options;
    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    std::unique_ptr<StreamableHttpMcpTransport> m_transport;
    std::mutex m_mutex;
    std::vector<Json> m_messages;
    std::vector<Error> m_errors;
    std::atomic<int> m_closes{0};
};

TEST_F(StreamableHttpMcpTransportTest, JsonReplyIsDeliveredAndSessionCaptured) {
    m_options.headers.emplace_back("X-Team", "pi");
    m_options.bearerToken = []() { return std::string("tok"); };
    build();
    m_http.enqueue(reply(200, "application/json; charset=utf-8", rpcResult(1).dump(),
                         {{"Mcp-Session-Id", "abc"}}));
    ASSERT_TRUE(m_transport->send(request(1, "initialize")).has_value());
    ASSERT_TRUE(waitUntil([&]() { return messages().size() == 1; }));
    EXPECT_EQ(messages()[0]["id"], 1);
    EXPECT_EQ(m_transport->sessionId(), "abc");
    const HttpRequest sent = m_http.requests()[0];
    EXPECT_EQ(sent.method, "POST");
    EXPECT_EQ(sent.url, "http://mcp.test/mcp");
    EXPECT_EQ(header(sent, "accept"), "application/json, text/event-stream");
    EXPECT_EQ(header(sent, "content-type"), "application/json");
    EXPECT_EQ(header(sent, "x-team"), "pi");
    EXPECT_EQ(header(sent, "authorization"), "Bearer tok");
    EXPECT_EQ(Json::parse(sent.body)["method"], "initialize");
}

TEST_F(StreamableHttpMcpTransportTest, LaterRequestsCarrySessionAndProtocolVersion) {
    build();
    m_http.enqueue(reply(200, "application/json", rpcResult(1).dump(), {{"mcp-session-id", "s1"}}));
    m_http.enqueue(reply(200, "application/json", rpcResult(2).dump()));
    ASSERT_TRUE(m_transport->send(request(1)).has_value());
    ASSERT_TRUE(waitUntil([&]() { return messages().size() == 1; }));
    m_transport->setProtocolVersion("2025-11-25");
    ASSERT_TRUE(m_transport->send(request(2)).has_value());
    ASSERT_TRUE(waitUntil([&]() { return messages().size() == 2; }));
    const HttpRequest second = m_http.requests()[1];
    EXPECT_EQ(header(second, "mcp-session-id"), "s1");
    EXPECT_EQ(header(second, "mcp-protocol-version"), "2025-11-25");
}

TEST_F(StreamableHttpMcpTransportTest, SseReplyDeliversEveryMessage) {
    build();
    const Json progress = Json{{"jsonrpc", "2.0"}, {"method", "notifications/progress"},
                               {"params", Json{{"progressToken", 1}, {"progress", 1}}}};
    m_http.enqueue(reply(200, "text/event-stream",
                         "data: " + progress.dump() + "\n\n" + "event: ping\ndata: x\n\n" +
                             ": comment\n\n" + "data: " + rpcResult(1).dump() + "\n\n"));
    ASSERT_TRUE(m_transport->send(request(1)).has_value());
    ASSERT_TRUE(waitUntil([&]() { return messages().size() == 2; }));
    EXPECT_EQ(messages()[0]["method"], "notifications/progress");
    EXPECT_EQ(messages()[1]["id"], 1);
    EXPECT_TRUE(errors().empty());
}

TEST_F(StreamableHttpMcpTransportTest, InvalidSseDataIsReportedAsAnError) {
    build();
    m_http.enqueue(reply(200, "text/event-stream",
                         "data: not json\n\ndata: " + rpcResult(1).dump() + "\n\n"));
    ASSERT_TRUE(m_transport->send(request(1)).has_value());
    ASSERT_TRUE(waitUntil([&]() { return messages().size() == 1 && errors().size() == 1; }));
    EXPECT_EQ(errors()[0].code, "protocol");
}

TEST_F(StreamableHttpMcpTransportTest, NotificationExpectsAcceptance) {
    build();
    m_http.enqueue(reply(202, "", ""));
    const Json note = Json{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}};
    EXPECT_TRUE(m_transport->send(note).has_value());
    m_http.enqueue(reply(500, "text/plain", "boom"));
    const auto failed = m_transport->send(note);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, "http:500");
    EXPECT_NE(failed.error().message.find("boom"), std::string::npos);
}

TEST_F(StreamableHttpMcpTransportTest, StatusErrorsAreClassified) {
    build();
    m_http.enqueue(reply(401, "text/plain", "no"));
    const auto unauthorized = m_transport->send(request(1));
    ASSERT_FALSE(unauthorized.has_value());
    EXPECT_EQ(unauthorized.error().code, "auth_required");

    m_http.enqueue(reply(200, "application/json", rpcResult(2).dump(), {{"Mcp-Session-Id", "s"}}));
    ASSERT_TRUE(m_transport->send(request(2)).has_value());
    ASSERT_TRUE(waitUntil([&]() { return messages().size() == 1; }));
    m_http.enqueue(reply(404, "text/plain", "gone"));
    const auto expired = m_transport->send(request(3));
    ASSERT_FALSE(expired.has_value());
    EXPECT_EQ(expired.error().code, "session_expired");
}

TEST_F(StreamableHttpMcpTransportTest, RequestsMustBeAnsweredWithAContentType) {
    build();
    m_http.enqueue(reply(202, "", ""));
    const auto accepted = m_transport->send(request(1));
    ASSERT_FALSE(accepted.has_value());
    EXPECT_EQ(accepted.error().code, "http:202");
    m_http.enqueue(reply(200, "text/html", "<html>"));
    const auto html = m_transport->send(request(2));
    ASSERT_FALSE(html.has_value());
    EXPECT_NE(html.error().message.find("text/html"), std::string::npos);
    m_http.enqueue(std::unexpected(Error{"transport", "down"}));
    const auto down = m_transport->send(request(3));
    ASSERT_FALSE(down.has_value());
    EXPECT_EQ(down.error().message, "down");
}

TEST_F(StreamableHttpMcpTransportTest, DroppedResponseStreamResumesWithLastEventId) {
    build();
    m_http.enqueue(reply(200, "text/event-stream", "id: 41\n\n"));
    m_http.enqueue(reply(200, "text/event-stream", "id: 42\ndata: " + rpcResult(1).dump() + "\n\n"));
    ASSERT_TRUE(m_transport->send(request(1)).has_value());
    ASSERT_TRUE(waitUntil([&]() { return messages().size() == 1; }));
    const auto sent = m_http.requests();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(sent[1].method, "GET");
    EXPECT_EQ(header(sent[1], "last-event-id"), "41");
    EXPECT_EQ(header(sent[1], "accept"), "text/event-stream");
    EXPECT_EQ(m_sleeper.delays(), std::vector<std::int64_t>{1000});
}

TEST_F(StreamableHttpMcpTransportTest, ServerRetryDelayIsHonoured) {
    build();
    m_http.enqueue(reply(200, "text/event-stream", "id: 1\nretry: 250\n\n"));
    m_http.enqueue(reply(200, "text/event-stream", "data: " + rpcResult(1).dump() + "\n\n"));
    ASSERT_TRUE(m_transport->send(request(1)).has_value());
    ASSERT_TRUE(waitUntil([&]() { return messages().size() == 1; }));
    EXPECT_EQ(m_sleeper.delays(), std::vector<std::int64_t>{250});
}

TEST_F(StreamableHttpMcpTransportTest, StreamWithoutEventIdsFailsTheRequest) {
    build();
    m_http.enqueue(reply(200, "text/event-stream", ""));
    ASSERT_TRUE(m_transport->send(request(7)).has_value());
    ASSERT_TRUE(waitUntil([&]() { return messages().size() == 1; }));
    EXPECT_EQ(messages()[0]["id"], 7);
    EXPECT_EQ(messages()[0]["error"]["code"], -32603);
    EXPECT_EQ(m_http.calls(), 1);
}

TEST_F(StreamableHttpMcpTransportTest, ResumeGivesUpAfterTheRetryBudget) {
    build();
    m_http.enqueue(reply(200, "text/event-stream", "id: 1\n\n"));
    m_http.enqueue(reply(500, "text/plain", "x"));
    m_http.enqueue(reply(500, "text/plain", "x"));
    ASSERT_TRUE(m_transport->send(request(1)).has_value());
    ASSERT_TRUE(waitUntil([&]() { return messages().size() == 1; }));
    EXPECT_EQ(messages()[0]["error"]["code"], -32603);
    EXPECT_EQ(m_http.calls(), 3);
    EXPECT_EQ(m_sleeper.delays(), (std::vector<std::int64_t>{1000, 2000}));
}

TEST_F(StreamableHttpMcpTransportTest, OversizedJsonBodyFailsTheRequest) {
    m_options.maxMessageBytes = 16;
    build();
    m_http.enqueue(reply(200, "application/json", rpcResult(1).dump()));
    ASSERT_TRUE(m_transport->send(request(1)).has_value());
    ASSERT_TRUE(waitUntil([&]() { return messages().size() == 1; }));
    EXPECT_NE(messages()[0]["error"]["message"].get<std::string>().find("exceeds 16 bytes"),
              std::string::npos);
}

TEST_F(StreamableHttpMcpTransportTest, InitializedOpensTheServerStream) {
    m_options.openGetStream = true;
    build();
    const Json note = Json{{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}};
    m_http.enqueue(reply(202, "", ""));
    m_http.enqueue(reply(200, "text/event-stream", "id: 9\ndata: " + note.dump() + "\n\n"));
    m_http.enqueue(reply(405, "", ""));
    ASSERT_TRUE(m_transport->send(Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}})
                    .has_value());
    ASSERT_TRUE(waitUntil([&]() { return messages().size() == 1; }));
    EXPECT_EQ(messages()[0]["method"], "notifications/tools/list_changed");
    ASSERT_TRUE(waitUntil([&]() { return m_http.calls() == 3; }));
    const auto sent = m_http.requests();
    EXPECT_EQ(sent[1].method, "GET");
    EXPECT_EQ(header(sent[2], "last-event-id"), "9");
    m_transport->close();
    EXPECT_EQ(m_http.calls(), 3);
}

TEST_F(StreamableHttpMcpTransportTest, ServerStreamNotOfferedStopsQuietly) {
    m_options.openGetStream = true;
    build();
    m_http.enqueue(reply(202, "", ""));
    m_http.enqueue(reply(405, "", ""));
    ASSERT_TRUE(m_transport->send(Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}})
                    .has_value());
    ASSERT_TRUE(waitUntil([&]() { return m_http.calls() == 2; }));
    m_transport->close();
    EXPECT_EQ(m_http.calls(), 2);
    EXPECT_TRUE(errors().empty());
}

TEST_F(StreamableHttpMcpTransportTest, ServerStreamReportsWhenItCannotBeReopened) {
    m_options.openGetStream = true;
    build();
    m_http.enqueue(reply(202, "", ""));
    for (int i = 0; i < 3; ++i) {
        m_http.enqueue(reply(200, "text/event-stream", ""));
    }
    ASSERT_TRUE(m_transport->send(Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}})
                    .has_value());
    ASSERT_TRUE(waitUntil([&]() { return errors().size() == 1; }));
    EXPECT_NE(errors()[0].message.find("could not be reopened"), std::string::npos);
}

TEST_F(StreamableHttpMcpTransportTest, CloseEndsTheSessionOnceAndEmitsClose) {
    build();
    m_http.enqueue(reply(200, "application/json", rpcResult(1).dump(), {{"Mcp-Session-Id", "s9"}}));
    m_http.enqueue(reply(200, "", ""));
    ASSERT_TRUE(m_transport->send(request(1)).has_value());
    ASSERT_TRUE(waitUntil([&]() { return messages().size() == 1; }));
    m_transport->close();
    m_transport->close();
    EXPECT_EQ(m_closes.load(), 1);
    const auto sent = m_http.requests();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(sent[1].method, "DELETE");
    EXPECT_EQ(header(sent[1], "mcp-session-id"), "s9");
    const auto after = m_transport->send(request(2));
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code, "closed");
}

TEST_F(StreamableHttpMcpTransportTest, CloseWithoutASessionSendsNothing) {
    build();
    m_transport->close();
    EXPECT_EQ(m_http.calls(), 0);
    EXPECT_EQ(m_closes.load(), 1);
}

TEST_F(StreamableHttpMcpTransportTest, SendBeforeStartIsRejected) {
    m_transport = std::make_unique<StreamableHttpMcpTransport>(m_http, m_sleeper, m_options);
    const auto result = m_transport->send(request(1));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "closed");
}

TEST_F(StreamableHttpMcpTransportTest, WorksUnderTheClient) {
    build(false);
    const Json initialized = Json{
        {"jsonrpc", "2.0"}, {"id", 1},
        {"result", Json{{"protocolVersion", "2025-11-25"}, {"capabilities", Json{{"tools", Json::object()}}},
                        {"serverInfo", Json{{"name", "http"}, {"version", "1"}}}}}};
    const Json tools = Json{{"jsonrpc", "2.0"}, {"id", 2},
                            {"result", Json{{"tools", Json::array({Json{{"name", "echo"},
                                                                         {"inputSchema", Json{{"type", "object"}}}}})}}}};
    m_http.enqueue(reply(200, "application/json", initialized.dump(), {{"Mcp-Session-Id", "c1"}}));
    m_http.enqueue(reply(202, "", ""));
    m_http.enqueue(reply(200, "text/event-stream", "data: " + tools.dump() + "\n\n"));
    m_http.enqueue(reply(200, "", ""));
    McpClientOptions clientOptions;
    clientOptions.requestTimeoutMs = 3000;
    McpClient client(clientOptions);
    std::unique_ptr<IMcpTransport> owned = std::move(m_transport);
    ASSERT_TRUE(client.connect(std::move(owned)).has_value());
    const auto listed = client.listTools({});
    ASSERT_TRUE(listed.has_value());
    ASSERT_EQ(listed->size(), 1U);
    EXPECT_EQ((*listed)[0].name, "echo");
    client.close();
    const auto sent = m_http.requests();
    ASSERT_EQ(sent.size(), 4U);
    EXPECT_EQ(header(sent[2], "mcp-session-id"), "c1");
    EXPECT_EQ(header(sent[2], "mcp-protocol-version"), "2025-11-25");
    EXPECT_EQ(sent[3].method, "DELETE");
}
