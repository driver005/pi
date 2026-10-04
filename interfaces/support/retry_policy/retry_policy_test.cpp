#include <gtest/gtest.h>

import std;
import pi.support.retry_policy;

class RetryPolicyTest : public testing::Test {
protected:
    HttpResponse response(int status, HttpHeaders headers = {}) {
        HttpResponse r;
        r.status = status;
        r.headers = std::move(headers);
        return r;
    }

    RetryPolicy m_policy;
};

TEST_F(RetryPolicyTest, RetryableStatuses) {
    for (const int status : {408, 409, 429, 500, 503}) {
        EXPECT_TRUE(m_policy.isRetryableStatus(response(status))) << status;
    }
    for (const int status : {400, 401, 403, 404}) {
        EXPECT_FALSE(m_policy.isRetryableStatus(response(status))) << status;
    }
}

TEST_F(RetryPolicyTest, ShouldRetryHeaderOverrides) {
    EXPECT_TRUE(m_policy.isRetryableStatus(response(400, {{"X-Should-Retry", "true"}})));
    EXPECT_FALSE(m_policy.isRetryableStatus(response(500, {{"x-should-retry", "false"}})));
}

TEST_F(RetryPolicyTest, TransportErrorsRetriedExceptAbort) {
    EXPECT_TRUE(m_policy.isRetryableTransportError(Error{"transport", "reset"}));
    EXPECT_TRUE(m_policy.isRetryableTransportError(Error{"timeout", "slow"}));
    EXPECT_FALSE(m_policy.isRetryableTransportError(Error{"aborted", "stop"}));
}

TEST_F(RetryPolicyTest, BackoffGrowsAndCaps) {
    EXPECT_EQ(m_policy.backoffMs(0, 0.0), 500);
    EXPECT_EQ(m_policy.backoffMs(1, 0.0), 1000);
    EXPECT_EQ(m_policy.backoffMs(2, 0.0), 2000);
    EXPECT_EQ(m_policy.backoffMs(10, 0.0), 8000);
    EXPECT_EQ(m_policy.backoffMs(0, 0.99), 376);
}

TEST_F(RetryPolicyTest, RetryAfterMsPreferred) {
    const auto delay = m_policy.delayMs(
        response(429, {{"retry-after-ms", "250"}, {"retry-after", "9"}}), 0, std::nullopt, 0, 0, "m");
    ASSERT_TRUE(delay.has_value());
    EXPECT_EQ(*delay, 250);
}

TEST_F(RetryPolicyTest, RetryAfterSeconds) {
    const auto delay =
        m_policy.delayMs(response(429, {{"retry-after", "2"}}), 0, std::nullopt, 0, 0, "m");
    EXPECT_EQ(*delay, 2000);
}

TEST_F(RetryPolicyTest, RetryAfterHttpDate) {
    // 1994-11-06 08:49:37 GMT = 784111777 s
    const auto delay = m_policy.delayMs(
        response(429, {{"retry-after", "Sun, 06 Nov 1994 08:49:37 GMT"}}), 0, std::nullopt,
        784111777000LL - 5000, 0, "m");
    ASSERT_TRUE(delay.has_value());
    EXPECT_EQ(*delay, 5000);
}

TEST_F(RetryPolicyTest, ServerDelayAboveCapIsError) {
    const auto delay =
        m_policy.delayMs(response(429, {{"retry-after", "120"}}), 0, std::nullopt, 0, 0, "busy");
    ASSERT_FALSE(delay.has_value());
    EXPECT_EQ(delay.error().message, "Server requested 120s retry delay (max: 60s). busy");
}

TEST_F(RetryPolicyTest, ZeroCapDisablesLimit) {
    const auto delay =
        m_policy.delayMs(response(429, {{"retry-after", "120"}}), 0, 0, 0, 0, "busy");
    EXPECT_EQ(*delay, 120000);
}
