#include <gtest/gtest.h>

import std;
import pi.support.jsonl_line_reader;

TEST(JsonlLineReaderTest, SplitsOnLfAcrossChunks) {
    JsonlLineReader reader;
    EXPECT_TRUE(reader.feed("{\"a\":").empty());
    const auto lines = reader.feed("1}\n{\"b\":2}\n{\"c\"");
    EXPECT_EQ(lines, (std::vector<std::string>{"{\"a\":1}", "{\"b\":2}"}));
    EXPECT_EQ(reader.feed(":3}\n"), (std::vector<std::string>{"{\"c\":3}"}));
    EXPECT_FALSE(reader.finish().has_value());
}

TEST(JsonlLineReaderTest, KeepsUnicodeSeparatorsInsideRecords) {
    JsonlLineReader reader;
    const auto lines = reader.feed("{\"t\":\"a\xE2\x80\xA8" "b\"}\n");
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find("\xE2\x80\xA8"), std::string::npos);
}

TEST(JsonlLineReaderTest, DropsCarriageReturnsAndEmptyLines) {
    JsonlLineReader reader;
    EXPECT_EQ(reader.feed("one\r\n\n\r\ntwo\n"), (std::vector<std::string>{"one", "two"}));
}

TEST(JsonlLineReaderTest, FinishReturnsTheUnterminatedTail) {
    JsonlLineReader reader;
    reader.feed("tail\r");
    EXPECT_EQ(reader.finish(), "tail");
    EXPECT_FALSE(reader.finish().has_value());
}
