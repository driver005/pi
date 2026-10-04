#include <gtest/gtest.h>

import std;
import pi.base.thread_sleeper;
import pi.support.abort_signal;

TEST(ThreadSleeperTest, SleepsForDuration) {
    ThreadSleeper sleeper;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(sleeper.sleep(std::chrono::milliseconds(30), nullptr));
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(25));
}

TEST(ThreadSleeperTest, AbortWakesEarly) {
    ThreadSleeper sleeper;
    auto signal = std::make_shared<AbortSignal>();
    std::thread aborter([signal]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        signal->abort();
    });
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(sleeper.sleep(std::chrono::seconds(30), signal));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
    aborter.join();
}

TEST(ThreadSleeperTest, AlreadyAbortedReturnsFalseImmediately) {
    ThreadSleeper sleeper;
    auto signal = std::make_shared<AbortSignal>();
    signal->abort();
    EXPECT_FALSE(sleeper.sleep(std::chrono::seconds(30), signal));
}
