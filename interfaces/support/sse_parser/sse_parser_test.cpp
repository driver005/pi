#include <gtest/gtest.h>

import std;
import pi.support.sse_parser;

TEST(SseParserTest, ParsesSingleEvent) {
    SseParser parser;
    const auto events = parser.feed("event: ping\ndata: hello\n\n");
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].event, "ping");
    EXPECT_EQ(events[0].data, "hello");
}

TEST(SseParserTest, DefaultsEventNameAndJoinsDataLines) {
    SseParser parser;
    const auto events = parser.feed("data: a\ndata: b\n\n");
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].event, "message");
    EXPECT_EQ(events[0].data, "a\nb");
}

TEST(SseParserTest, HandlesFragmentedChunksAndCrLf) {
    SseParser parser;
    EXPECT_TRUE(parser.feed("da").empty());
    EXPECT_TRUE(parser.feed("ta: x\r").empty());
    const auto events = parser.feed("\n\r\ndata: y\r\n\r\n");
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].data, "x");
    EXPECT_EQ(events[1].data, "y");
}

TEST(SseParserTest, IgnoresCommentsAndEventsWithoutData) {
    SseParser parser;
    const auto events = parser.feed(": keepalive\n\nevent: noop\n\ndata: z\n\n");
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].data, "z");
}

TEST(SseParserTest, TracksIdAndRetry) {
    SseParser parser;
    const auto events = parser.feed("id: 7\nretry: 1500\ndata: q\n\n");
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].id, "7");
    EXPECT_EQ(events[0].retryMs, 1500);
}

TEST(SseParserTest, FinishFlushesUnterminatedEvent) {
    SseParser parser;
    EXPECT_TRUE(parser.feed("data: tail").empty());
    const auto events = parser.finish();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].data, "tail");
}
