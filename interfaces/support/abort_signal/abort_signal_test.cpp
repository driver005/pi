#include "interfaces/support/abort_signal/abort_signal.h"

#include <gtest/gtest.h>

TEST(AbortSignalTest, StartsNotAborted) {
    AbortSignal signal;
    EXPECT_FALSE(signal.aborted());
}

TEST(AbortSignalTest, AbortRunsListenersOnce) {
    AbortSignal signal;
    int calls = 0;
    signal.onAbort([&] { ++calls; });
    signal.abort();
    signal.abort();
    EXPECT_TRUE(signal.aborted());
    EXPECT_EQ(calls, 1);
}

TEST(AbortSignalTest, ListenerAddedAfterAbortRunsImmediately) {
    AbortSignal signal;
    signal.abort();
    int calls = 0;
    signal.onAbort([&] { ++calls; });
    EXPECT_EQ(calls, 1);
}

TEST(AbortSignalTest, RemovedListenerDoesNotRun) {
    AbortSignal signal;
    int calls = 0;
    const auto id = signal.onAbort([&] { ++calls; });
    signal.removeListener(id);
    signal.abort();
    EXPECT_EQ(calls, 0);
}
