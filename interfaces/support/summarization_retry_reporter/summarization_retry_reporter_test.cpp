#include <gtest/gtest.h>

import std;
import pi.support.summarization_retry_reporter;
import pi.testing.recording_session_sink;

TEST(SummarizationRetryReporterTest, MapsCallbacksToEvents) {
    RecordingSessionSink sink;
    SummarizationRetryReporter reporter(sink);
    const RetryCallbacks callbacks = reporter.callbacks("compaction", "threshold");
    callbacks.onRetryScheduled(1, 3, 250, "overloaded");
    callbacks.onRetryAttemptStart();
    callbacks.onRetryFinished(true, 1, std::nullopt);
    const auto events = sink.events();
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0].type, SessionEventType::SummarizationRetryScheduled);
    EXPECT_EQ(events[0].attempt, 1);
    EXPECT_EQ(events[0].maxAttempts, 3);
    EXPECT_EQ(events[0].delayMs, 250);
    EXPECT_EQ(events[0].errorMessage, "overloaded");
    EXPECT_EQ(events[1].type, SessionEventType::SummarizationRetryAttemptStart);
    EXPECT_EQ(events[1].source, "compaction");
    EXPECT_EQ(events[1].reason, "threshold");
    EXPECT_EQ(events[2].type, SessionEventType::SummarizationRetryFinished);
}
