#include "interfaces/support/frontmatter_parser/frontmatter_parser.h"

#include <gtest/gtest.h>

class FrontmatterParserTest : public testing::Test {
protected:
    FrontmatterParser m_parser;
};

TEST_F(FrontmatterParserTest, ParsesHeaderAndBody) {
    const auto doc = m_parser.parse("---\nname: demo\ndescription: A skill\n---\nBody text\n");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->frontmatter["name"], "demo");
    EXPECT_EQ(doc->frontmatter["description"], "A skill");
    EXPECT_EQ(doc->body, "Body text\n");
}

TEST_F(FrontmatterParserTest, NoHeaderReturnsWholeBody) {
    const auto doc = m_parser.parse("just text\n");
    ASSERT_TRUE(doc.has_value());
    EXPECT_TRUE(doc->frontmatter.empty());
    EXPECT_EQ(doc->body, "just text\n");
}

TEST_F(FrontmatterParserTest, HandlesBomAndCrLf) {
    const auto doc = m_parser.parse("\xEF\xBB\xBF---\r\nname: x\r\n---\r\nbody\r\n");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->frontmatter["name"], "x");
    EXPECT_EQ(doc->body, "body\n");
}

TEST_F(FrontmatterParserTest, TypesScalarsAndLists) {
    const auto doc = m_parser.parse(
        "---\nflag: true\ncount: 3\nratio: 1.5\nitems:\n  - a\n  - b\nquoted: \"true\"\n---\n");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->frontmatter["flag"], true);
    EXPECT_EQ(doc->frontmatter["count"], 3);
    EXPECT_EQ(doc->frontmatter["ratio"], 1.5);
    EXPECT_EQ(doc->frontmatter["items"], Json::parse(R"(["a","b"])"));
    EXPECT_EQ(doc->frontmatter["quoted"], "true");
}

TEST_F(FrontmatterParserTest, MalformedYamlReturnsErrorInsteadOfThrowing) {
    const auto doc = m_parser.parse("---\nname: [unclosed\n---\nbody\n");
    EXPECT_FALSE(doc.has_value());
}

TEST_F(FrontmatterParserTest, UnterminatedHeaderTreatedAsBody) {
    const auto doc = m_parser.parse("---\nname: x\nno end\n");
    ASSERT_TRUE(doc.has_value());
    EXPECT_TRUE(doc->frontmatter.empty());
}
