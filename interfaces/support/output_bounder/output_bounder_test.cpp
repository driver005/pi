#include <gtest/gtest.h>

import std;
import pi.support.output_bounder;

class OutputBounderTest : public ::testing::Test {
protected:
    OutputLimits limits(std::int64_t bytes, std::int64_t lines, const std::string& retain) {
        OutputLimits result;
        result.maxBytes = bytes;
        result.maxLines = lines;
        result.retain = retain;
        return result;
    }

    OutputBounder m_bounder;
};

TEST_F(OutputBounderTest, SanitizeDropsControlCharactersAndKeepsTabsAndNewlines) {
    EXPECT_EQ(m_bounder.sanitize(std::string("a\x00\x07" "b\tc\nd\x1b" "e\r", 11)), "ab\tc\nde");
    EXPECT_EQ(m_bounder.sanitize("x\xEF\xBF\xB9y\xEF\xBF\xBBz\xEF\xBF\xBDw"), "xyz\xEF\xBF\xBDw");
    EXPECT_EQ(m_bounder.sanitize("héllo ✓"), "héllo ✓");
}

TEST_F(OutputBounderTest, HeadKeepsTheFirstLinesWithinBothLimits) {
    OutputSlice byLines = m_bounder.bound("a\nb\nc\nd\n", limits(1000, 2, "head"));
    EXPECT_EQ(byLines.text, "a\nb\n");
    EXPECT_EQ(byLines.droppedLines, 2);
    EXPECT_EQ(byLines.droppedBytes, 4);
    OutputSlice byBytes = m_bounder.bound("aaaa\nbbbb\ncccc\n", limits(11, 100, "head"));
    EXPECT_EQ(byBytes.text, "aaaa\nbbbb\n");
    EXPECT_EQ(byBytes.bytes, 10);
    EXPECT_EQ(m_bounder.bound("short\n", limits(100, 100, "head")).droppedBytes, 0);
}

TEST_F(OutputBounderTest, TailKeepsTheLastLines) {
    EXPECT_EQ(m_bounder.bound("a\nb\nc\nd\n", limits(1000, 2, "tail")).text, "c\nd\n");
    EXPECT_EQ(m_bounder.bound("a\nb\nc\nd", limits(1000, 2, "tail")).text, "c\nd");
    OutputSlice byBytes = m_bounder.bound("aaaa\nbbbb\ncccc\n", limits(11, 100, "tail"));
    EXPECT_EQ(byBytes.text, "bbbb\ncccc\n");
}

TEST_F(OutputBounderTest, AnOverlongLineIsCutOnACharacterBoundary) {
    // "é" is two bytes; a byte limit inside it backs off to the previous boundary (head) or forward (tail).
    OutputSlice head = m_bounder.bound("aé" "bcdef", limits(2, 100, "head"));
    EXPECT_EQ(head.text, "a");
    OutputSlice tail = m_bounder.bound("abcdé" "f", limits(2, 100, "tail"));
    EXPECT_EQ(tail.text, "f");
    EXPECT_EQ(m_bounder.bound("abc", limits(0, 10, "head")).text, "");
    EXPECT_EQ(m_bounder.bound("abc", limits(10, 0, "tail")).text, "");
}

TEST_F(OutputBounderTest, EmptyTextBoundsToEmpty) {
    OutputSlice slice = m_bounder.bound("", limits(10, 10, "tail"));
    EXPECT_EQ(slice.text, "");
    EXPECT_EQ(slice.droppedLines, 0);
}
