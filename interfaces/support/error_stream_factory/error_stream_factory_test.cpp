#include <gtest/gtest.h>

import std;
import pi.support.error_stream_factory;

TEST(ErrorStreamFactoryTest, StreamEndsWithErrorMessage) {
    ErrorStreamFactory factory;
    Model model;
    model.id = "m";
    model.api = "faux";
    model.provider = "p";
    auto stream = factory.failed(model, "boom", 42);
    const auto event = stream->next();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, AssistantEventType::Error);
    EXPECT_FALSE(stream->next().has_value());
    const auto result = stream->result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->errorMessage, "boom");
    EXPECT_EQ(result->stopReason, StopReason::Error);
    EXPECT_EQ(result->provider, "p");
    EXPECT_EQ(result->timestamp, 42);
}
