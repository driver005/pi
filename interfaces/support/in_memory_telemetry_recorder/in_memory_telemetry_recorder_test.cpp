#include <gtest/gtest.h>

import std;
import pi.support.in_memory_telemetry_recorder;

TEST(InMemoryTelemetryRecorderTest, RecordsSpansInStartOrderAndSettlesOnce) {
    InMemoryTelemetryRecorder recorder;
    const auto a = recorder.begin(std::nullopt, SpanOptions{"a", Json{{"k", "v"}, {"dropped", nullptr}}});
    const auto b = recorder.begin(a, SpanOptions{"b", Json::object()});
    recorder.settle(b, std::nullopt);
    recorder.settle(a, Error{"E", "m"});
    recorder.settle(a, std::nullopt);
    const auto spans = recorder.spans();
    ASSERT_EQ(spans.size(), 2U);
    EXPECT_EQ(spans[0].attributes, (Json{{"k", "v"}}));
    EXPECT_EQ(spans[1].parentId, a);
    EXPECT_EQ(spans[1].endSequence, 1);
    EXPECT_EQ(spans[0].endSequence, 2);
    EXPECT_FALSE(spans[0].status.ok);
    EXPECT_EQ(spans[0].status.errorName, "E");
    EXPECT_EQ(spans[0].status.errorMessage, "m");
    EXPECT_TRUE(recorder.settled(a));
}

TEST(InMemoryTelemetryRecorderTest, ExplicitStatusWinsAndSettledSpansAreInert) {
    InMemoryTelemetryRecorder recorder;
    const auto id = recorder.begin(std::nullopt, SpanOptions{"s", Json::object()});
    recorder.setStatus(id, SpanStatus{});
    recorder.addEvent(id, "e", Json{{"x", 1}, {"y", nullptr}});
    recorder.setAttributes(id, Json{{"n", 1}});
    recorder.settle(id, Error{"E", "late failure"});
    recorder.addEvent(id, "late", Json::object());
    recorder.setAttributes(id, Json{{"n", 2}});
    recorder.setStatus(id, SpanStatus{false, std::nullopt, std::nullopt});
    const auto span = recorder.spans()[0];
    EXPECT_TRUE(span.status.ok);
    ASSERT_EQ(span.events.size(), 1U);
    EXPECT_EQ(span.events[0].attributes, (Json{{"x", 1}}));
    EXPECT_EQ(span.attributes, (Json{{"n", 1}}));
}
