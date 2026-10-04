#include <gtest/gtest.h>

import std;
import pi.support.skill_command_expander;
import pi.testing.fake_file_system;

class SkillCommandExpanderTest : public testing::Test {
protected:
    SkillCommandExpanderTest() {
        m_files.createDirectories("/skills/review");
        m_files.writeFile("/skills/review/SKILL.md", "---\nname: review\ndescription: d\n---\n\nCheck the diff.\n");
        Skill skill;
        skill.name = "review";
        skill.filePath = "/skills/review/SKILL.md";
        skill.baseDir = "/skills/review";
        m_skills.push_back(skill);
        Skill missing = skill;
        missing.name = "gone";
        missing.filePath = "/skills/gone/SKILL.md";
        m_skills.push_back(missing);
    }

    FakeFileSystem m_files;
    std::vector<Skill> m_skills;
    SkillCommandExpander m_expander{m_files};
};

TEST_F(SkillCommandExpanderTest, ExpandsSkillWithArguments) {
    const auto result = m_expander.expand("/skill:review the auth change", m_skills);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result,
              "<skill name=\"review\" location=\"/skills/review/SKILL.md\">\nReferences are relative to /skills/review.\n\nCheck the diff.\n</skill>\n\nthe auth change");
}

TEST_F(SkillCommandExpanderTest, ExpandsWithoutArguments) {
    const auto result = m_expander.expand("/skill:review", m_skills);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ends_with("Check the diff.\n</skill>"));
}

TEST_F(SkillCommandExpanderTest, PassesThroughOtherTextAndUnknownSkills) {
    EXPECT_EQ(*m_expander.expand("hello", m_skills), "hello");
    EXPECT_EQ(*m_expander.expand("/skill:unknown x", m_skills), "/skill:unknown x");
}

TEST_F(SkillCommandExpanderTest, UnreadableSkillFileIsAnError) {
    EXPECT_FALSE(m_expander.expand("/skill:gone", m_skills).has_value());
}

TEST_F(SkillCommandExpanderTest, ParsesBlockRoundTrip) {
    const auto text = *m_expander.expand("/skill:review the auth change", m_skills);
    const auto block = m_expander.parse(text);
    ASSERT_TRUE(block.has_value());
    EXPECT_EQ(block->name, "review");
    EXPECT_EQ(block->location, "/skills/review/SKILL.md");
    EXPECT_NE(block->content.find("Check the diff."), std::string::npos);
    EXPECT_EQ(block->userMessage, "the auth change");
}

TEST_F(SkillCommandExpanderTest, ParseRejectsNonBlocks) {
    EXPECT_FALSE(m_expander.parse("plain text").has_value());
    EXPECT_FALSE(m_expander.parse("<skill name=\"a\" location=\"b\">\nno close").has_value());
    const auto bare = m_expander.parse("<skill name=\"a\" location=\"b\">\nbody\n</skill>");
    ASSERT_TRUE(bare.has_value());
    EXPECT_FALSE(bare->userMessage.has_value());
}
