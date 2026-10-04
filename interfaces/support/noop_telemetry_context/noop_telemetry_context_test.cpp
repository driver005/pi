#include <gtest/gtest.h>

import std;
import pi.support.noop_telemetry_context;

TEST(NoopTelemetryContextTest, AdmitsOnceSynchronouslyAndPassesTheResultThrough) {
    NoopTelemetryContext context;
    int calls = 0;
    EXPECT_TRUE(context.startSpan(SpanOptions{"ok", Json::object()}, [&](ITelemetrySpan&) -> Result<void> {
        ++calls;
        return {};
    }).has_value());
    EXPECT_EQ(calls, 1);
    const auto failed = context.startSpan(SpanOptions{"bad", Json::object()}, [](ITelemetrySpan&) -> Result<void> { return std::unexpected(Error{"E", "nope"}); });
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, "E");
}
