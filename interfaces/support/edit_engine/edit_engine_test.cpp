#include <gtest/gtest.h>

import std;
import pi.support.edit_engine;

class EditEngineTest : public testing::Test {
protected:
    Result<AppliedEdits> apply(const std::string& content, std::vector<TextEdit> edits) {
        return m_engine.applyEdits(content, edits, "f.txt");
    }

    EditEngine m_engine;
};

TEST_F(EditEngineTest, LineEndingHelpers) {
    EXPECT_EQ(m_engine.detectLineEnding("a\r\nb\n"), "\r\n");
    EXPECT_EQ(m_engine.detectLineEnding("a\nb\r\n"), "\n");
    EXPECT_EQ(m_engine.detectLineEnding("abc"), "\n");
    EXPECT_EQ(m_engine.normalizeToLF("a\r\nb\rc"), "a\nb\nc");
    EXPECT_EQ(m_engine.restoreLineEndings("a\nb", "\r\n"), "a\r\nb");
    const auto bom = m_engine.splitBom("\xEF\xBB\xBF" "x");
    EXPECT_EQ(bom.first, "\xEF\xBB\xBF");
    EXPECT_EQ(bom.second, "x");
}

TEST_F(EditEngineTest, ExactReplacement) {
    const auto result = apply("hello world\nsecond\n", {{"world", "there"}});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->newContent, "hello there\nsecond\n");
    EXPECT_EQ(result->baseContent, "hello world\nsecond\n");
}

TEST_F(EditEngineTest, MultipleDisjointEditsMatchOriginalContent) {
    const auto result = apply("a\nb\nc\n", {{"a", "b"}, {"c", "a"}});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->newContent, "b\nb\na\n");
}

TEST_F(EditEngineTest, NotFoundDuplicateEmptyAndOverlapErrors) {
    EXPECT_NE(apply("abc", {{"zzz", "x"}}).error().message.find("Could not find the exact text in f.txt"),
              std::string::npos);
    EXPECT_NE(apply("aa aa", {{"aa", "x"}}).error().message.find("Found 2 occurrences"), std::string::npos);
    EXPECT_NE(apply("abc", {{"", "x"}}).error().message.find("oldText must not be empty"), std::string::npos);
    EXPECT_NE(apply("abcdef", {{"abcd", "1"}, {"cdef", "2"}}).error().message.find("overlap"), std::string::npos);
    EXPECT_NE(apply("abc", {{"b", "b"}}).error().message.find("No changes made"), std::string::npos);
    EXPECT_NE(apply("abc", {{"zzz", "x"}, {"a", "b"}}).error().message.find("edits[0]"), std::string::npos);
}

TEST_F(EditEngineTest, FuzzyMatchesSmartQuotesAndTrailingWhitespacePreservingOtherLines) {
    const std::string content = "keep  \nsay \xE2\x80\x9Chi\xE2\x80\x9D  \nlast\n";
    const auto result = apply(content, {{"say \"hi\"", "say \"yo\""}});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->newContent, "keep  \nsay \"yo\"\nlast\n");
}

TEST_F(EditEngineTest, FuzzyNormalizationMapsDashesSpacesAndFullwidth) {
    EXPECT_EQ(m_engine.normalizeForFuzzyMatch("a\xE2\x80\x94" "b\xC2\xA0" "c  \n"), "a-b c\n");
    EXPECT_EQ(m_engine.normalizeForFuzzyMatch("\xEF\xBC\xA1"), "A");
    EXPECT_EQ(m_engine.normalizeForFuzzyMatch("wait\xE2\x80\xA6"), "wait...");
}

TEST_F(EditEngineTest, FuzzyFindReportsWhichSpaceOffsetsLiveIn) {
    const auto exact = m_engine.fuzzyFindText("abc def", "def");
    EXPECT_TRUE(exact.found);
    EXPECT_FALSE(exact.usedFuzzyMatch);
    EXPECT_EQ(exact.index, 4U);
    const auto fuzzy = m_engine.fuzzyFindText("it\xE2\x80\x99s", "it's");
    EXPECT_TRUE(fuzzy.found);
    EXPECT_TRUE(fuzzy.usedFuzzyMatch);
    EXPECT_FALSE(m_engine.fuzzyFindText("abc", "xyz").found);
}

TEST_F(EditEngineTest, MultiLineOldTextIsMatchedAfterLfNormalization) {
    const auto result = apply("one\ntwo\nthree\n", {{"one\r\ntwo", "1\n2"}});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->newContent, "1\n2\nthree\n");
}
