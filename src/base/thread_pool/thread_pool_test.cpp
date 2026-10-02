#include "src/base/thread_pool/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>

TEST(ThreadPoolTest, RunsAllSubmittedTasksBeforeDestruction) {
    std::atomic<int> done{0};
    {
        ThreadPool pool(4);
        for (int i = 0; i < 200; ++i) {
            pool.submit([&] { ++done; });
        }
    }
    EXPECT_EQ(done.load(), 200);
}

TEST(ThreadPoolTest, TasksRunConcurrently) {
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    {
        ThreadPool pool(3);
        for (int i = 0; i < 3; ++i) {
            pool.submit([&] {
                const int now = ++running;
                int seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                --running;
            });
        }
    }
    EXPECT_GE(peak.load(), 2);
}

TEST(ThreadPoolTest, ZeroWorkersStillGetsOneThread) {
    std::atomic<bool> ran{false};
    {
        ThreadPool pool(0);
        pool.submit([&] { ran = true; });
    }
    EXPECT_TRUE(ran.load());
}
