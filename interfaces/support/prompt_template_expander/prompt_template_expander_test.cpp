#include <gtest/gtest.h>

import std;
import pi.support.prompt_template_expander;

class PromptTemplateExpanderTest : public testing::Test {
protected:
    PromptTemplate templ(const std::string& name, const std::string& content) {
        PromptTemplate t;
        t.name = name;
        t.content = content;
        return t;
    }

    PromptTemplateExpander m_expander;
};

TEST_F(PromptTemplateExpanderTest, ParsesQuotedArguments) {
    EXPECT_EQ(m_expander.parseArgs("a 'b c' \"d e\"  f"), (std::vector<std::string>{"a", "b c", "d e", "f"}));
    EXPECT_EQ(m_expander.parseArgs("  "), std::vector<std::string>{});
    EXPECT_EQ(m_expander.parseArgs("x'y z'w"), (std::vector<std::string>{"xy zw"}));
}

TEST_F(PromptTemplateExpanderTest, PositionalAndAllPlaceholders) {
    const std::vector<std::string> args = {"one", "two", "three"};
    EXPECT_EQ(m_expander.substitute("$1-$2-$3-$4|", args), "one-two-three-|");
    EXPECT_EQ(m_expander.substitute("$@|$ARGUMENTS", args), "one two three|one two three");
    EXPECT_EQ(m_expander.substitute("$10", args), "");
}

TEST_F(PromptTemplateExpanderTest, DefaultsAndSlices) {
    const std::vector<std::string> args = {"a", "b", "c", "d"};
    EXPECT_EQ(m_expander.substitute("${1:-x} ${9:-fallback} ${@:-none}", args), "a fallback a b c d");
    EXPECT_EQ(m_expander.substitute("${@:-none}", {}), "none");
    EXPECT_EQ(m_expander.substitute("${@:2}", args), "b c d");
    EXPECT_EQ(m_expander.substitute("${@:2:2}", args), "b c");
    EXPECT_EQ(m_expander.substitute("${@:0}", args), "a b c d");
    EXPECT_EQ(m_expander.substitute("${ARGUMENTS:-q}", {}), "q");
}

TEST_F(PromptTemplateExpanderTest, ValuesAreNotRescanned) {
    EXPECT_EQ(m_expander.substitute("$1", {"$2 and $@"}), "$2 and $@");
    EXPECT_EQ(m_expander.substitute("${1:-$2}", {}), "$2");
}

TEST_F(PromptTemplateExpanderTest, NonPlaceholdersAreLeftAlone) {
    EXPECT_EQ(m_expander.substitute("cost $ and ${unclosed and $x and ${a:b}", {"v"}), "cost $ and ${unclosed and $x and ${a:b}");
}

TEST_F(PromptTemplateExpanderTest, ExpandsKnownTemplatesOnly) {
    const std::vector<PromptTemplate> templates = {templ("review", "Review $1 carefully: $@")};
    EXPECT_EQ(m_expander.expand("/review main.cpp 'extra bit'", templates), "Review main.cpp carefully: main.cpp extra bit");
    EXPECT_EQ(m_expander.expand("/review", templates), "Review  carefully: ");
    EXPECT_EQ(m_expander.expand("/unknown x", templates), "/unknown x");
    EXPECT_EQ(m_expander.expand("plain text", templates), "plain text");
    EXPECT_EQ(m_expander.expand("/", templates), "/");
}
