#include <gtest/gtest.h>

import std;
import pi.support.in_memory_telemetry_context;
import pi.support.typed_span_starter;

TEST(TypedSpanStarterTest, ChildSpansNestUnderTheSpanThatStartedThem) {
    InMemoryTelemetryContext recording;
    TypedSpanStarter starter(recording);
    ASSERT_TRUE(starter.start("agent.run", Json{{"model", "m"}}, [](ITelemetrySpan& span, TypedSpanStarter& child) -> Result<void> {
        span.setAttributes(Json{{"turns", 2}});
        return child.start("agent.turn", Json::object(), [](ITelemetrySpan&, TypedSpanStarter&) -> Result<void> { return {}; });
    }).has_value());
    const auto spans = recording.getSpans();
    ASSERT_EQ(spans.size(), 2U);
    EXPECT_EQ(spans[0].name, "agent.run");
    EXPECT_EQ(spans[0].attributes, (Json{{"model", "m"}, {"turns", 2}}));
    EXPECT_EQ(spans[1].parentId, spans[0].id);
}
