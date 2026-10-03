#include <gtest/gtest.h>

#include <csignal>
#include <unistd.h>

import std;
import pi.base.posix_signal_waiter;

TEST(PosixSignalWaiterTest, ReturnsTheSignalThatArrived) {
    PosixSignalWaiter waiter;
    waiter.block();
    std::thread sender([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        kill(getpid(), SIGTERM);
    });
    EXPECT_EQ(waiter.wait(), SIGTERM);
    sender.join();
}

TEST(PosixSignalWaiterTest, SignalsSentBeforeWaitingAreNotLost) {
    PosixSignalWaiter waiter;
    waiter.block();
    kill(getpid(), SIGHUP);
    EXPECT_EQ(waiter.wait(), SIGHUP);
}
