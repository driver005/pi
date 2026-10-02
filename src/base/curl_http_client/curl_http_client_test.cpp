#include <gtest/gtest.h>

import std;
import pi.base.curl_http_client;
import pi.testing.fake_http_server;

class CurlHttpClientTest : public testing::Test {
protected:
    CurlHttpClient m_client;
};

TEST_F(CurlHttpClientTest, GetReturnsStatusHeadersAndBody) {
    FakeHttpServer server([](const FakeHttpRequest&) {
        FakeHttpReply reply;
        reply.headers = {{"X-Test", "yes"}, {"Content-Length", "5"}};
        reply.chunks = {"hello"};
        return reply;
    });
    HttpRequest request;
    request.url = server.url("/a");
    const auto response = m_client.send(request);
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->body, "hello");
    bool found = false;
    for (const auto& [name, value] : response->headers) {
        found = found || (name == "X-Test" && value == "yes");
    }
    EXPECT_TRUE(found);
}

TEST_F(CurlHttpClientTest, PostSendsMethodHeadersAndBody) {
    FakeHttpServer server([](const FakeHttpRequest&) {
        FakeHttpReply reply;
        reply.chunks = {"ok"};
        return reply;
    });
    HttpRequest request;
    request.method = "POST";
    request.url = server.url("/v1/messages");
    request.headers = {{"Content-Type", "application/json"}, {"x-api-key", "k"}};
    request.body = "{\"a\":1}";
    ASSERT_TRUE(m_client.send(request).has_value());
    const auto seen = server.requests();
    ASSERT_EQ(seen.size(), 1U);
    EXPECT_EQ(seen[0].method, "POST");
    EXPECT_EQ(seen[0].path, "/v1/messages");
    EXPECT_EQ(seen[0].body, "{\"a\":1}");
    bool hasKey = false;
    for (const auto& [name, value] : seen[0].headers) {
        hasKey = hasKey || (name == "x-api-key" && value == "k");
    }
    EXPECT_TRUE(hasKey);
}

TEST_F(CurlHttpClientTest, StreamsSuccessBodyAndCollectsErrorBody) {
    FakeHttpServer server([](const FakeHttpRequest& request) {
        FakeHttpReply reply;
        if (request.path == "/fail") {
            reply.status = 500;
            reply.chunks = {"bad"};
        } else {
            reply.chunks = {"one", "two", "three"};
            reply.chunkDelayMs = 20;
        }
        return reply;
    });
    HttpRequest request;
    request.url = server.url("/stream");
    std::string streamed;
    request.onBody = [&](std::string_view chunk) { streamed.append(chunk); };
    const auto ok = m_client.send(request);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(streamed, "onetwothree");
    EXPECT_TRUE(ok->body.empty());

    HttpRequest failing = request;
    failing.url = server.url("/fail");
    streamed.clear();
    const auto failed = m_client.send(failing);
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(failed->status, 500);
    EXPECT_EQ(failed->body, "bad");
    EXPECT_TRUE(streamed.empty());
}

TEST_F(CurlHttpClientTest, AbortInterruptsStream) {
    FakeHttpServer server([](const FakeHttpRequest&) {
        FakeHttpReply reply;
        reply.chunks = {"a"};
        reply.tailDelayMs = 10000;
        return reply;
    });
    HttpRequest request;
    request.url = server.url("/slow");
    request.signal = std::make_shared<AbortSignal>();
    request.onBody = [](std::string_view) {};
    std::thread aborter([signal = request.signal] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        signal->abort();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto response = m_client.send(request);
    aborter.join();
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, "aborted");
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
}

TEST_F(CurlHttpClientTest, OverallTimeoutReportsTimeout) {
    FakeHttpServer server([](const FakeHttpRequest&) {
        FakeHttpReply reply;
        reply.initialDelayMs = 3000;
        return reply;
    });
    HttpRequest request;
    request.url = server.url("/");
    request.timeout = std::chrono::milliseconds(300);
    const auto response = m_client.send(request);
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, "timeout");
}

TEST_F(CurlHttpClientTest, IdleTimeoutReportsTimeout) {
    FakeHttpServer server([](const FakeHttpRequest&) {
        FakeHttpReply reply;
        reply.chunks = {"x"};
        reply.tailDelayMs = 5000;
        return reply;
    });
    HttpRequest request;
    request.url = server.url("/");
    request.idleTimeout = std::chrono::milliseconds(300);
    request.onBody = [](std::string_view) {};
    const auto response = m_client.send(request);
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, "timeout");
}

TEST_F(CurlHttpClientTest, ConnectionRefusedIsTransportError) {
    HttpRequest request;
    request.url = "http://127.0.0.1:1/";
    const auto response = m_client.send(request);
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, "http_transport");
}

TEST_F(CurlHttpClientTest, OnResponseFiresBeforeBodyChunks) {
    FakeHttpServer server([](const FakeHttpRequest&) {
        FakeHttpReply reply;
        reply.headers = {{"X-Mark", "m"}};
        reply.chunks = {"abc"};
        return reply;
    });
    std::vector<std::string> order;
    HttpRequest request;
    request.url = server.url("/a");
    request.onResponse = [&](int status, const HttpHeaders& headers) {
        bool marked = false;
        for (const auto& header : headers) {
            marked = marked || header.first == "X-Mark";
        }
        order.push_back("response:" + std::to_string(status) + (marked ? ":marked" : ""));
    };
    request.onBody = [&](std::string_view chunk) { order.push_back("body:" + std::string(chunk)); };
    ASSERT_TRUE(m_client.send(request).has_value());
    ASSERT_EQ(order.size(), 2U);
    EXPECT_EQ(order[0], "response:200:marked");
    EXPECT_EQ(order[1], "body:abc");
}
