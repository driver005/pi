#include <gtest/gtest.h>

import std;
import pi.support.output_sanitizer;

class OutputSanitizerTest : public testing::Test {
protected:
    OutputSanitizer m_sanitizer;
};

TEST_F(OutputSanitizerTest, PlainTextPassesThroughAndCarriageReturnsGo) {
    EXPECT_EQ(m_sanitizer.feed("line one\r\nline two\n"), "line one\nline two\n");
}

TEST_F(OutputSanitizerTest, StripsColorAndCursorSequences) {
    EXPECT_EQ(m_sanitizer.feed("\x1B[31mred\x1B[0m \x1B[2K\x1B[1Gdone"), "red done");
}

TEST_F(OutputSanitizerTest, StripsOscTitleSequences) {
    EXPECT_EQ(m_sanitizer.feed("a\x1B]0;window title\x07" "b\x1B]8;;http://x\x1B\\link"), "ablink");
}

TEST_F(OutputSanitizerTest, DropsControlAndBinaryCharactersButKeepsTabsAndNewlines) {
    EXPECT_EQ(m_sanitizer.feed(std::string("a\x00\x01\x07\tb\nc\x0B\x0C\x1F", 12)), "a\tb\nc");
}

TEST_F(OutputSanitizerTest, HoldsBackSplitUtf8Sequences) {
    const std::string heart = "\xE2\x9D\xA4";
    std::string out = m_sanitizer.feed("x" + heart.substr(0, 1));
    out += m_sanitizer.feed(heart.substr(1) + "y");
    EXPECT_EQ(out, "x" + heart + "y");
}

TEST_F(OutputSanitizerTest, TruncatedEscapeIsNotAllowedToEatText) {
    EXPECT_EQ(m_sanitizer.feed("text\x1B] free"), "text] free");
}
