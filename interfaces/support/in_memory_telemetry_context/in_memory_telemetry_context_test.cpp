#include <gtest/gtest.h>

import std;
import pi.support.in_memory_telemetry_context;

/** The cases of the adapter conformance suite of packages/telemetry that do not depend on JavaScript values. */
class InMemoryTelemetryContextTest : public testing::Test {
protected:
    RecordedTelemetrySpan find(const std::string& name) {
        for (const RecordedTelemetrySpan& span : m_context.getSpans()) {
            if (span.name == name) {
                return span;
            }
        }
        ADD_FAILURE() << "no span " << name;
        return {};
    }

    InMemoryTelemetryContext m_context;
};

TEST_F(InMemoryTelemetryContextTest, AdmitsOnceSynchronouslyAndSettlesOk) {
    int calls = 0;
    ASSERT_TRUE(m_context.startSpan(SpanOptions{"success", Json::object()}, [&](ITelemetrySpan&) -> Result<void> {
        ++calls;
        return {};
    }).has_value());
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(find("success").status.ok);
    EXPECT_TRUE(find("success").settled);
}

TEST_F(InMemoryTelemetryContextTest, FailuresReachTheCallerAndBecomeTheStatus) {
    const auto result = m_context.startSpan(SpanOptions{"failing", Json::object()}, [](ITelemetrySpan&) -> Result<void> { return std::unexpected(Error{"Boom", "it broke"}); });
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "it broke");
    EXPECT_FALSE(find("failing").status.ok);
    EXPECT_EQ(find("failing").status.errorName, "Boom");
    EXPECT_EQ(find("failing").status.errorMessage, "it broke");
}

TEST_F(InMemoryTelemetryContextTest, UsesTheLastExplicitStatusWithoutAutomaticOverwrite) {
    ASSERT_TRUE(m_context.startSpan(SpanOptions{"last-status", Json::object()}, [](ITelemetrySpan& span) -> Result<void> {
        span.setStatus(SpanStatus{false, "Expected", "first"});
        span.setStatus(SpanStatus{});
        return {};
    }).has_value());
    EXPECT_FALSE(m_context.startSpan(SpanOptions{"explicit-before-failure", Json::object()}, [](ITelemetrySpan& span) -> Result<void> {
        span.setStatus(SpanStatus{});
        return std::unexpected(Error{"E", "after explicit status"});
    }).has_value());
    EXPECT_TRUE(m_context.startSpan(SpanOptions{"expected-failure", Json::object()}, [](ITelemetrySpan& span) -> Result<void> {
        span.setStatus(SpanStatus{false, "Expected", "returned failure"});
        return {};
    }).has_value());
    EXPECT_TRUE(find("last-status").status.ok);
    EXPECT_TRUE(find("explicit-before-failure").status.ok);
    EXPECT_FALSE(find("expected-failure").status.ok);
    EXPECT_EQ(find("expected-failure").status.errorMessage, "returned failure");
}

TEST_F(InMemoryTelemetryContextTest, MergesAttributesAndRecordsOrderedEvents) {
    ASSERT_TRUE(m_context.startSpan(SpanOptions{"recording", Json{{"start", "value"}, {"overwrite", "start"}, {"ignored", nullptr}}}, [](ITelemetrySpan& span) -> Result<void> {
        span.setAttributes(Json{{"count", 1}, {"overwrite", "middle"}});
        span.setAttributes(Json{{"count", nullptr}, {"overwrite", "end"}});
        span.addEvent("first", Json{{"index", 1}, {"ignored", nullptr}});
        span.addEvent("second", Json{{"index", 2}});
        return {};
    }).has_value());
    const RecordedTelemetrySpan span = find("recording");
    EXPECT_EQ(span.attributes, (Json{{"start", "value"}, {"overwrite", "end"}, {"count", 1}}));
    ASSERT_EQ(span.events.size(), 2U);
    EXPECT_EQ(span.events[0].name, "first");
    EXPECT_EQ(span.events[0].attributes, (Json{{"index", 1}}));
    EXPECT_EQ(span.events[1].attributes, (Json{{"index", 2}}));
}

TEST_F(InMemoryTelemetryContextTest, RecordsNestedParentageAndSettlementOrder) {
    ASSERT_TRUE(m_context.startSpan(SpanOptions{"parent", Json::object()}, [](ITelemetrySpan& parent) -> Result<void> {
        auto first = parent.startChild(SpanOptions{"first-child", Json::object()}, [](ITelemetrySpan&) -> Result<void> { return {}; });
        auto second = parent.startChild(SpanOptions{"second-child", Json::object()}, [](ITelemetrySpan&) -> Result<void> { return {}; });
        return first && second ? Result<void>{} : std::unexpected(Error{"E", "child failed"});
    }).has_value());
    const RecordedTelemetrySpan parent = find("parent");
    const RecordedTelemetrySpan first = find("first-child");
    const RecordedTelemetrySpan second = find("second-child");
    EXPECT_FALSE(parent.parentId.has_value());
    EXPECT_EQ(first.parentId, parent.id);
    EXPECT_EQ(second.parentId, parent.id);
    ASSERT_TRUE(first.endSequence && second.endSequence && parent.endSequence);
    EXPECT_LT(*first.endSequence, *second.endSequence);
    EXPECT_LT(*second.endSequence, *parent.endSequence);
}

TEST_F(InMemoryTelemetryContextTest, ConcurrentSpansAreRecordedWithoutLoss) {
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 50; ++i) {
                (void)m_context.startSpan(SpanOptions{"work", Json::object()}, [](ITelemetrySpan& span) -> Result<void> {
                    span.addEvent("e", Json::object());
                    return {};
                });
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(m_context.getSpans().size(), 200U);
}
