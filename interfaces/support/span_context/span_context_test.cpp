#include <gtest/gtest.h>

import std;
import pi.support.in_memory_telemetry_context;
import pi.support.span_context;

TEST(SpanContextTest, SpansStartedThroughASpanContextAreItsChildren) {
    InMemoryTelemetryContext recording;
    ASSERT_TRUE(recording.startSpan(SpanOptions{"parent", Json::object()}, [](ITelemetrySpan& parent) -> Result<void> {
        SpanContext context(parent);
        return context.startSpan(SpanOptions{"child", Json::object()}, [](ITelemetrySpan&) -> Result<void> { return {}; });
    }).has_value());
    const auto spans = recording.getSpans();
    ASSERT_EQ(spans.size(), 2U);
    EXPECT_EQ(spans[1].parentId, spans[0].id);
}
