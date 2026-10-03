#include <gtest/gtest.h>

import std;
import pi.support.json_waiters;

TEST(JsonWaitersTest, ResolveReleasesEveryWaiterOfAKey) {
    JsonWaiters waiters;
    auto first = waiters.add(7);
    auto second = waiters.add(7);
    auto other = waiters.add(8);
    waiters.resolve(7, Json::object({{"ok", true}}));
    EXPECT_EQ(waiters.await(first)->at("ok"), true);
    EXPECT_EQ(waiters.await(second)->at("ok"), true);
    EXPECT_EQ(waiters.keys(), std::vector<std::int64_t>({8}));
    waiters.rejectAll(Error{"closed", "gone"});
    auto rejected = waiters.await(other);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code, "closed");
}

TEST(JsonWaitersTest, AbortSettlesOnlyThatWait) {
    JsonWaiters waiters;
    AbortSignal signal;
    auto cancelled = waiters.add(1, &signal);
    auto kept = waiters.add(1);
    std::thread aborter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        signal.abort();
    });
    auto result = waiters.await(cancelled);
    aborter.join();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "aborted");
    waiters.resolve(1, Json(5));
    EXPECT_EQ(*waiters.await(kept), 5);
}

TEST(JsonWaitersTest, AlreadyAbortedSignalSettlesAtAdd) {
    JsonWaiters waiters;
    AbortSignal signal;
    signal.abort();
    EXPECT_FALSE(waiters.await(waiters.add(1, &signal)).has_value());
    EXPECT_TRUE(waiters.keys().empty());
}

TEST(JsonWaitersTest, ResolveAcrossThreads) {
    JsonWaiters waiters;
    auto slot = waiters.add(3);
    std::thread resolver([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        waiters.resolve(3, Json("done"));
    });
    EXPECT_EQ(*waiters.await(slot), "done");
    resolver.join();
}
