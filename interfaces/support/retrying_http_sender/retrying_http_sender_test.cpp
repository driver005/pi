#include <gtest/gtest.h>

import std;
import pi.support.retrying_http_sender;
import pi.testing.fixed_clock;
import pi.testing.recording_sleeper;
import pi.testing.scripted_http_client;

class RetryingHttpSenderTest : public testing::Test {
protected:
    HttpResponse reply(int status, HttpHeaders headers = {}, std::string body = "") {
        HttpResponse r;
        r.status = status;
        r.headers = std::move(headers);
        r.body = std::move(body);
        return r;
    }

    ScriptedHttpClient m_http;
    RecordingSleeper m_sleeper;
    FixedClock m_clock;
    RetryingHttpSender m_sender{m_http, m_sleeper, m_clock};
};

TEST_F(RetryingHttpSenderTest, ReturnsSuccessWithoutRetry) {
    m_http.enqueue(reply(200));
    const auto result = m_sender.send(HttpRequest{}, 3, std::nullopt);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(m_http.calls(), 1);
    EXPECT_TRUE(m_sleeper.delays().empty());
}

TEST_F(RetryingHttpSenderTest, RetriesRetryableStatusThenSucceeds) {
    m_http.enqueue(reply(503, {{"retry-after-ms", "10"}}));
    m_http.enqueue(reply(429, {{"retry-after-ms", "20"}}));
    m_http.enqueue(reply(200));
    const auto result = m_sender.send(HttpRequest{}, 3, std::nullopt);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(m_http.calls(), 3);
    EXPECT_EQ(m_sleeper.delays(), (std::vector<std::int64_t>{10, 20}));
}

TEST_F(RetryingHttpSenderTest, ReturnsLastFailureWhenRetriesExhausted) {
    m_http.enqueue(reply(500, {{"retry-after-ms", "1"}}));
    m_http.enqueue(reply(500, {{"retry-after-ms", "1"}}));
    const auto result = m_sender.send(HttpRequest{}, 1, std::nullopt);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, 500);
    EXPECT_EQ(m_http.calls(), 2);
}

TEST_F(RetryingHttpSenderTest, DoesNotRetryClientErrors) {
    m_http.enqueue(reply(400));
    const auto result = m_sender.send(HttpRequest{}, 3, std::nullopt);
    EXPECT_EQ(result->status, 400);
    EXPECT_EQ(m_http.calls(), 1);
}

TEST_F(RetryingHttpSenderTest, ServerDelayAboveCapFails) {
    m_http.enqueue(reply(429, {{"retry-after", "300"}}, "rate limited"));
    const auto result = m_sender.send(HttpRequest{}, 3, std::nullopt);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("300s retry delay"), std::string::npos);
    EXPECT_NE(result.error().message.find("rate limited"), std::string::npos);
}

TEST_F(RetryingHttpSenderTest, RetriesTransportErrors) {
    m_http.enqueue(std::unexpected(Error{"transport", "reset"}));
    m_http.enqueue(reply(200));
    const auto result = m_sender.send(HttpRequest{}, 2, std::nullopt);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(m_sleeper.delays().size(), 1U);
}

TEST_F(RetryingHttpSenderTest, AbortDuringBackoffReturnsAbortError) {
    m_http.enqueue(reply(500, {{"retry-after-ms", "1"}}));
    m_sleeper.abortOnNextSleep();
    HttpRequest request;
    request.signal = std::make_shared<AbortSignal>();
    const auto result = m_sender.send(request, 3, std::nullopt);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "aborted");
}
