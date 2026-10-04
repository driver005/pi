#include <gtest/gtest.h>

import std;
import pi.support.plugin_stream_runs;

TEST(PluginStreamRunsTest, CloseCancelsRunningStreamsAndWaitsForThem) {
    PluginStreamRuns runs;
    auto abort = std::make_shared<AbortSignal>();
    ASSERT_TRUE(runs.begin(abort));
    EXPECT_EQ(runs.running(), 1U);
    std::atomic<bool> ended = false;
    std::thread worker([&] {
        while (!abort->aborted()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ended = true;
        runs.end(abort);
    });
    runs.close();
    EXPECT_TRUE(ended.load());
    EXPECT_EQ(runs.running(), 0U);
    worker.join();
}

TEST(PluginStreamRunsTest, RefusesStreamsAfterClose) {
    PluginStreamRuns runs;
    runs.close();
    EXPECT_FALSE(runs.begin(std::make_shared<AbortSignal>()));
}
