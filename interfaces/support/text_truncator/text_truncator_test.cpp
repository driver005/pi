#include <gtest/gtest.h>

import std;
import pi.support.text_truncator;

class TextTruncatorTest : public testing::Test {
protected:
    std::string lines(int count) {
        std::string out;
        for (int i = 1; i <= count; ++i) {
            out += "line" + std::to_string(i) + (i < count ? "\n" : "");
        }
        return out;
    }

    TextTruncator m_truncator;
};

TEST_F(TextTruncatorTest, FormatSize) {
    EXPECT_EQ(m_truncator.formatSize(512), "512B");
    EXPECT_EQ(m_truncator.formatSize(1536), "1.5KB");
    EXPECT_EQ(m_truncator.formatSize(5 * 1024 * 1024), "5.0MB");
}

TEST_F(TextTruncatorTest, HeadWithinLimitsIsUntouched) {
    const auto result = m_truncator.truncateHead("a\nb\nc\n");
    EXPECT_FALSE(result.truncated);
    EXPECT_EQ(result.content, "a\nb\nc\n");
    EXPECT_EQ(result.totalLines, 3);
}

TEST_F(TextTruncatorTest, HeadTruncatesByLines) {
    const auto result = m_truncator.truncateHead(lines(10), 3, 1000);
    EXPECT_TRUE(result.truncated);
    EXPECT_EQ(result.truncatedBy, "lines");
    EXPECT_EQ(result.content, "line1\nline2\nline3");
    EXPECT_EQ(result.outputLines, 3);
    EXPECT_EQ(result.totalLines, 10);
}

TEST_F(TextTruncatorTest, HeadTruncatesByBytesWithoutPartialLines) {
    const auto result = m_truncator.truncateHead(lines(10), 100, 14);
    EXPECT_EQ(result.truncatedBy, "bytes");
    EXPECT_EQ(result.content, "line1\nline2");
}

TEST_F(TextTruncatorTest, HeadFirstLineTooLong) {
    const auto result = m_truncator.truncateHead(std::string(100, 'x') + "\nnext", 100, 50);
    EXPECT_TRUE(result.firstLineExceedsLimit);
    EXPECT_TRUE(result.content.empty());
    EXPECT_EQ(result.truncatedBy, "bytes");
}

TEST_F(TextTruncatorTest, TailKeepsLastLines) {
    const auto result = m_truncator.truncateTail(lines(10), 3, 1000);
    EXPECT_EQ(result.content, "line8\nline9\nline10");
    EXPECT_EQ(result.truncatedBy, "lines");
}

TEST_F(TextTruncatorTest, TailPartialLastLineOnUtf8Boundary) {
    std::string overlong;
    for (int i = 0; i < 20; ++i) {
        overlong += "\xc3\xa9";
    }
    const auto result = m_truncator.truncateTail("a\n" + overlong, 100, 9);
    EXPECT_TRUE(result.lastLinePartial);
    EXPECT_LE(result.content.size(), 9U);
    EXPECT_EQ(result.content.size() % 2, 0U);
}

TEST_F(TextTruncatorTest, TruncateLineCutsAtCharacters) {
    const auto cut = m_truncator.truncateLine("abcdef", 3);
    EXPECT_TRUE(cut.second);
    EXPECT_EQ(cut.first, "abc... [truncated]");
    EXPECT_FALSE(m_truncator.truncateLine("abc", 3).second);
    EXPECT_EQ(m_truncator.truncateLine("\xc3\xa9\xc3\xa9\xc3\xa9", 2).first, "\xc3\xa9\xc3\xa9... [truncated]");
}
