#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.oauth_device_poller;
import pi.testing.fixed_clock;

/** A sleeper that moves the clock instead of waiting. */
class AdvancingSleeper : public ISleeper {
public:
    explicit AdvancingSleeper(FixedClock& clock)
        : m_clock(clock) {}

    bool sleep(std::chrono::milliseconds duration, const std::shared_ptr<AbortSignal>& signal) override {
        m_delays.push_back(duration.count());
        m_clock.advance(duration.count());
        return !(signal && signal->aborted());
    }

    std::vector<std::int64_t> m_delays;

private:
    FixedClock& m_clock;
};

class OauthDevicePollerTest : public testing::Test {
protected:
    DevicePollResult pending() {
        return DevicePollResult{};
    }

    DevicePollResult slowDown(std::optional<std::int64_t> interval = std::nullopt) {
        DevicePollResult result;
        result.status = "slow_down";
        result.intervalSeconds = interval;
        return result;
    }

    DevicePollResult complete(const Json& value) {
        DevicePollResult result;
        result.status = "complete";
        result.value = value;
        return result;
    }

    FixedClock m_clock{1'000'000};
    AdvancingSleeper m_sleeper{m_clock};
    OauthDevicePoller m_poller{m_clock, m_sleeper};
};

TEST_F(OauthDevicePollerTest, PollsEveryIntervalUntilTheFlowCompletes) {
    int attempts = 0;
    const auto result = m_poller.poll(2, 600, false, nullptr, [&] { return ++attempts < 3 ? pending() : complete(Json{{"token", "t"}}); });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)["token"], "t");
    EXPECT_EQ(attempts, 3);
    EXPECT_EQ(m_sleeper.m_delays, (std::vector<std::int64_t>{2000, 2000}));
}

TEST_F(OauthDevicePollerTest, TheFirstPollCanWaitAnIntervalAndTheDefaultIsFiveSeconds) {
    const auto result = m_poller.poll(std::nullopt, 600, true, nullptr, [&] { return complete(Json::object()); });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(m_sleeper.m_delays, std::vector<std::int64_t>{5000});
    const auto fast = m_poller.poll(0, std::nullopt, false, nullptr, [&] { return complete(Json::object()); });
    EXPECT_TRUE(fast.has_value());
}

TEST_F(OauthDevicePollerTest, SlowDownTakesTheServersIntervalOrAddsFiveSeconds) {
    int attempts = 0;
    const auto result = m_poller.poll(1, 600, false, nullptr, [&] {
        ++attempts;
        if (attempts == 1) {
            return slowDown();
        }
        if (attempts == 2) {
            return slowDown(20);
        }
        return complete(Json::object());
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(m_sleeper.m_delays, (std::vector<std::int64_t>{6000, 20000}));
}

TEST_F(OauthDevicePollerTest, AFailedPollEndsTheFlowWithItsMessage) {
    const auto result = m_poller.poll(1, 600, false, nullptr, [&] {
        DevicePollResult failed;
        failed.status = "failed";
        failed.message = "denied";
        return failed;
    });
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "oauth");
    EXPECT_EQ(result.error().message, "denied");
}

TEST_F(OauthDevicePollerTest, TheFlowExpires) {
    const auto result = m_poller.poll(5, 12, false, nullptr, [&] { return pending(); });
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "device_timeout");
    EXPECT_EQ(result.error().message, "Device flow timed out");
    int attempts = 0;
    const auto drifting = m_poller.poll(1, 8, false, nullptr, [&] { return ++attempts == 1 ? slowDown() : pending(); });
    ASSERT_FALSE(drifting.has_value());
    EXPECT_NE(drifting.error().message.find("clock drift"), std::string::npos);
}

TEST_F(OauthDevicePollerTest, ACancelledSignalEndsTheFlow) {
    const auto signal = std::make_shared<AbortSignal>();
    int attempts = 0;
    const auto result = m_poller.poll(1, 600, false, signal, [&] {
        if (++attempts == 2) {
            signal->abort();
        }
        return pending();
    });
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "login_cancelled");
}
