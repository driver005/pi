#include <gtest/gtest.h>

import std;
import pi.support.noop_telemetry_span;

TEST(NoopTelemetrySpanTest, RunsChildrenInlineAndIgnoresRecording) {
    NoopTelemetrySpan span;
    int calls = 0;
    const auto result = span.startChild(SpanOptions{"child", Json::object()}, [&](ITelemetrySpan& child) -> Result<void> {
        ++calls;
        child.addEvent("e", Json{{"a", 1}});
        child.setAttributes(Json{{"b", 2}});
        child.setStatus(SpanStatus{false, "Boom", "m"});
        return {};
    });
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(calls, 1);
    const auto failing = span.startChild(SpanOptions{"c", Json::object()}, [](ITelemetrySpan&) -> Result<void> { return std::unexpected(Error{"E", "failed"}); });
    ASSERT_FALSE(failing.has_value());
    EXPECT_EQ(failing.error().message, "failed");
}
