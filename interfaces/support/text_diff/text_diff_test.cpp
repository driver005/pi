#include <gtest/gtest.h>

import std;
import pi.support.text_diff;

class TextDiffTest : public testing::Test {
protected:
    TextDiff m_diff;
};

TEST_F(TextDiffTest, IdenticalTextHasSingleCommonPart) {
    const auto parts = m_diff.diffLines("a\nb\n", "a\nb\n");
    ASSERT_EQ(parts.size(), 1U);
    EXPECT_FALSE(parts[0].added || parts[0].removed);
    EXPECT_EQ(parts[0].lines, (std::vector<std::string>{"a", "b"}));
}

TEST_F(TextDiffTest, ReplacementPutsRemovalBeforeAddition) {
    const auto parts = m_diff.diffLines("a\nb\nc\n", "a\nX\nc\n");
    ASSERT_EQ(parts.size(), 4U);
    EXPECT_TRUE(parts[1].removed);
    EXPECT_EQ(parts[1].lines, (std::vector<std::string>{"b"}));
    EXPECT_TRUE(parts[2].added);
    EXPECT_EQ(parts[2].lines, (std::vector<std::string>{"X"}));
}

TEST_F(TextDiffTest, InsertionsAndDeletions) {
    const auto parts = m_diff.diffLines("a\nc\n", "a\nb\nc\nd\n");
    int added = 0;
    for (const auto& part : parts) {
        added += part.added ? static_cast<int>(part.lines.size()) : 0;
    }
    EXPECT_EQ(added, 2);
    const auto deleted = m_diff.diffLines("a\nb\nc\n", "");
    ASSERT_EQ(deleted.size(), 1U);
    EXPECT_TRUE(deleted[0].removed);
    EXPECT_EQ(deleted[0].lines.size(), 3U);
}

TEST_F(TextDiffTest, UnifiedPatchMatchesJsdiffFormat) {
    const std::string patch = m_diff.unifiedPatch("f.txt", "a\nb\nc\nd\ne\n", "a\nb\nX\nd\ne\n", 1);
    EXPECT_EQ(patch,
              "--- f.txt\n+++ f.txt\n@@ -2,3 +2,3 @@\n b\n-c\n+X\n d\n");
}

TEST_F(TextDiffTest, UnifiedPatchSplitsFarApartChangesIntoHunks) {
    std::string oldText;
    std::string newText;
    for (int i = 1; i <= 30; ++i) {
        oldText += "line" + std::to_string(i) + "\n";
        newText += (i == 2 || i == 29 ? "CHANGED" + std::to_string(i) : "line" + std::to_string(i)) + "\n";
    }
    const std::string patch = m_diff.unifiedPatch("f", oldText, newText, 2);
    std::size_t hunks = 0;
    for (std::size_t pos = patch.find("@@ -"); pos != std::string::npos; pos = patch.find("@@ -", pos + 1)) {
        ++hunks;
    }
    EXPECT_EQ(hunks, 2U);
}

TEST_F(TextDiffTest, NoNewlineAtEndOfFileMarker) {
    const std::string patch = m_diff.unifiedPatch("f", "a\nb", "a\nc", 1);
    EXPECT_NE(patch.find("\\ No newline at end of file"), std::string::npos);
}

TEST_F(TextDiffTest, DisplayDiffNumbersLinesAndTracksFirstChange) {
    const auto result = m_diff.displayDiff("a\nb\nc\n", "a\nX\nc\n");
    EXPECT_EQ(result.firstChangedLine, std::optional<int>(2));
    EXPECT_EQ(result.diff, " 1 a\n-2 b\n+2 X\n 3 c");
}

TEST_F(TextDiffTest, DisplayDiffElidesLongContext) {
    std::string oldText;
    std::string newText;
    for (int i = 1; i <= 40; ++i) {
        oldText += "l" + std::to_string(i) + "\n";
        newText += (i == 1 || i == 40 ? "X" : "l" + std::to_string(i)) + "\n";
    }
    const auto result = m_diff.displayDiff(oldText, newText);
    EXPECT_NE(result.diff.find(" ..."), std::string::npos);
    EXPECT_EQ(result.firstChangedLine, std::optional<int>(1));
}
