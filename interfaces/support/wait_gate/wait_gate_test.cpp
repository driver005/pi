#include <gtest/gtest.h>

import std;
import pi.support.wait_gate;

TEST(WaitGateTest, OpenReleasesWaiters) {
    WaitGate gate;
    std::thread opener([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        gate.open();
    });
    EXPECT_TRUE(gate.wait().has_value());
    opener.join();
    EXPECT_TRUE(gate.isOpen());
    EXPECT_TRUE(gate.wait().has_value());
}

TEST(WaitGateTest, AbortReleasesWaiterWithAbortedError) {
    WaitGate gate;
    AbortSignal signal;
    std::thread aborter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        signal.abort();
    });
    auto waited = gate.wait({&signal});
    aborter.join();
    ASSERT_FALSE(waited.has_value());
    EXPECT_EQ(waited.error().code, "aborted");
    EXPECT_FALSE(gate.isOpen());
}

TEST(WaitGateTest, AlreadyAbortedSignalFailsImmediately) {
    WaitGate gate;
    AbortSignal signal;
    signal.abort();
    EXPECT_FALSE(gate.wait({&signal}).has_value());
}

TEST(WaitGateTest, OpenWinsOverUnabortedSignal) {
    WaitGate gate;
    AbortSignal signal;
    gate.open();
    EXPECT_TRUE(gate.wait({&signal}).has_value());
}

TEST(WaitGateTest, WaitForTimesOutThenSeesOpenAndAbort) {
    WaitGate gate;
    AbortSignal signal;
    auto timedOut = gate.waitFor(std::chrono::milliseconds(10), {&signal});
    ASSERT_TRUE(timedOut.has_value());
    EXPECT_FALSE(*timedOut);
    signal.abort();
    EXPECT_FALSE(gate.waitFor(std::chrono::milliseconds(10), {&signal}).has_value());
    gate.open();
    auto opened = gate.waitFor(std::chrono::milliseconds(10));
    ASSERT_TRUE(opened.has_value());
    EXPECT_TRUE(*opened);
}
