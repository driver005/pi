#include <gtest/gtest.h>

import std;
import pi.support.http_date_parser;

TEST(HttpDateParserTest, ParsesImfFixdate) {
    HttpDateParser parser;
    EXPECT_EQ(parser.parseMs("Sun, 06 Nov 1994 08:49:37 GMT"), 784111777000LL);
    EXPECT_EQ(parser.parseMs("Thu, 01 Jan 1970 00:00:00 GMT"), 0);
}

TEST(HttpDateParserTest, RejectsGarbage) {
    HttpDateParser parser;
    EXPECT_FALSE(parser.parseMs("tomorrow").has_value());
    EXPECT_FALSE(parser.parseMs("Sun, 31 Feb 1994 08:49:37 GMT").has_value());
    EXPECT_FALSE(parser.parseMs("Sun, 06 Foo 1994 08:49:37 GMT").has_value());
    EXPECT_FALSE(parser.parseMs("").has_value());
}
