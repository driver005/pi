#include <gtest/gtest.h>

import std;
import pi.support.progress_publisher;

TEST(ProgressPublisherTest, FirstMarkCommitsAtOnceAndLaterOnesAreThrottled) {
    std::mutex mutex;
    std::vector<std::chrono::steady_clock::time_point> times;
    ProgressPublisher publisher(
        [&]() -> Result<std::int64_t> {
            const std::lock_guard<std::mutex> lock(mutex);
            times.push_back(std::chrono::steady_clock::now());
            return 10;
        },
        [](const Error&) {});
    const auto begin = std::chrono::steady_clock::now();
    publisher.mark();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    publisher.mark();
    publisher.mark();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    (void)publisher.stop();
    const std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(times.size(), 2u);
    EXPECT_LT(times[0] - begin, std::chrono::milliseconds(50));
    // The second commit waited for the 100 ms minimum interval after the first.
    EXPECT_GE(times[1] - times[0], std::chrono::milliseconds(95));
}

TEST(ProgressPublisherTest, MarkAndWaitSettlesWithTheCommitThatIncludesTheChange) {
    std::atomic<int> commits{0};
    ProgressPublisher publisher([&]() -> Result<std::int64_t> { return ++commits; }, [](const Error&) {});
    auto slot = publisher.markAndWait();
    EXPECT_TRUE(publisher.await(slot).has_value());
    EXPECT_GE(commits.load(), 1);
}

TEST(ProgressPublisherTest, FailuresRejectWaitersAndAreReported) {
    std::atomic<int> reported{0};
    ProgressPublisher publisher([]() -> Result<std::int64_t> { return std::unexpected(Error{"io", "write failed"}); },
                                [&](const Error&) { ++reported; });
    auto slot = publisher.markAndWait();
    auto result = publisher.await(slot);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "io");
    // The waiter settles before the failure is reported; stopping joins the publisher thread.
    (void)publisher.stop();
    EXPECT_EQ(reported.load(), 1);
}

TEST(ProgressPublisherTest, StopReturnsWaitersTheFinalCommitMustSettle) {
    std::atomic<int> commits{0};
    ProgressPublisher publisher([&]() -> Result<std::int64_t> { return ++commits; }, [](const Error&) {});
    publisher.mark();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // Within the throttle window, so this wait is still pending when the publisher stops.
    auto slot = publisher.markAndWait();
    auto pending = publisher.stop();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0], slot);
    publisher.resolve(pending[0]);
    EXPECT_TRUE(publisher.await(slot).has_value());
    // A stopped publisher takes no more marks.
    publisher.mark();
    EXPECT_EQ(commits.load(), 1);
}
