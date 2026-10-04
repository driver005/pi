#include <gtest/gtest.h>

import std;
import pi.support.in_memory_telemetry_span;

TEST(InMemoryTelemetrySpanTest, SettledSpansStartInertChildren) {
    InMemoryTelemetryRecorder recorder;
    InMemoryTelemetrySpan root(recorder, 0);
    ITelemetrySpan* captured = nullptr;
    std::int64_t capturedId = 0;
    ASSERT_TRUE(root.startChild(SpanOptions{"outer", Json::object()}, [&](ITelemetrySpan& span) -> Result<void> {
        captured = &span;
        capturedId = 1;
        return {};
    }).has_value());
    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(capturedId, 1);
    EXPECT_TRUE(recorder.settled(1));
    InMemoryTelemetrySpan settled(recorder, 1);
    int ran = 0;
    EXPECT_TRUE(settled.startChild(SpanOptions{"late", Json::object()}, [&](ITelemetrySpan&) -> Result<void> {
        ++ran;
        return {};
    }).has_value());
    EXPECT_EQ(ran, 1);
    EXPECT_EQ(recorder.spans().size(), 1U);
}
