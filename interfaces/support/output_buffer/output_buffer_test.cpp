#include <gtest/gtest.h>

import std;
import pi.support.output_buffer;

class OutputBufferTest : public ::testing::Test {
protected:
    OutputLimits limits(std::int64_t bytes, std::int64_t lines, const std::string& retain) {
        OutputLimits result;
        result.maxBytes = bytes;
        result.maxLines = lines;
        result.retain = retain;
        return result;
    }
};

TEST_F(OutputBufferTest, UnderTheLimitsEverythingIsRetained) {
    OutputBuffer buffer(limits(1000, 100, "head"));
    EXPECT_TRUE(buffer.push("hello "));
    EXPECT_TRUE(buffer.push("world\n"));
    EXPECT_FALSE(buffer.push(""));
    BoundedOutput output = buffer.snapshot();
    EXPECT_EQ(output.text, "hello world\n");
    EXPECT_EQ(output.droppedBytes, 0);
    EXPECT_EQ(output.droppedLines, 0);
}

TEST_F(OutputBufferTest, HeadKeepsTheStartAndCountsTheWholeStream) {
    OutputBuffer buffer(limits(1000, 2, "head"));
    for (int i = 0; i < 10; ++i) {
        buffer.push("line" + std::to_string(i) + "\n");
    }
    BoundedOutput output = buffer.snapshot();
    EXPECT_EQ(output.text, "line0\nline1\n");
    EXPECT_EQ(output.droppedLines, 8);
    EXPECT_EQ(output.droppedBytes, 8 * 6);
    // Nothing past a full window is stored.
    EXPECT_LT(buffer.storedBytes(), 30);
}

TEST_F(OutputBufferTest, TailKeepsTheEndAndBoundsStoredText) {
    OutputBuffer buffer(limits(1000, 3, "tail"));
    for (int i = 0; i < 1000; ++i) {
        buffer.push("row" + std::to_string(i) + "\n");
    }
    EXPECT_LT(buffer.storedBytes(), 200);
    BoundedOutput output = buffer.snapshot();
    EXPECT_EQ(output.text, "row997\nrow998\nrow999\n");
    EXPECT_EQ(output.droppedLines, 997);
}

TEST_F(OutputBufferTest, SnapshotsAreRepeatableAndIncremental) {
    OutputBuffer buffer(limits(1000, 2, "tail"));
    buffer.push("a\nb\n");
    EXPECT_EQ(buffer.snapshot().text, "a\nb\n");
    buffer.push("c\n");
    BoundedOutput output = buffer.snapshot();
    EXPECT_EQ(output.text, "b\nc\n");
    EXPECT_EQ(output.droppedLines, 1);
    EXPECT_EQ(buffer.snapshot().text, "b\nc\n");
}

TEST_F(OutputBufferTest, SplitCharactersJoinAcrossChunksAndTruncatedOnesBecomeReplacements) {
    OutputBuffer buffer(limits(1000, 100, "head"));
    buffer.push(std::string("caf\xC3"));
    EXPECT_EQ(buffer.snapshot().text, "caf");
    buffer.push(std::string("\xA9!"));
    EXPECT_EQ(buffer.snapshot().text, "caf\xC3\xA9!");
    buffer.push(std::string("\xE2\x9C"));
    buffer.end();
    EXPECT_EQ(buffer.snapshot().text, "caf\xC3\xA9!\xEF\xBF\xBD");
}

TEST_F(OutputBufferTest, SnapshotSanitizesControlCharacters) {
    OutputBuffer buffer(limits(1000, 100, "head"));
    buffer.push(std::string("a\x07" "b\n"));
    EXPECT_EQ(buffer.snapshot().text, "ab\n");
}
