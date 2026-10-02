#include <gtest/gtest.h>

import std;
import pi.support.skill_prompt_formatter;

TEST(SkillPromptFormatterTest, FormatsVisibleSkillsAndEscapes) {
    Skill visible;
    visible.name = "pdf";
    visible.description = "Use <pdf> & friends";
    visible.filePath = "/s/pdf/SKILL.md";
    Skill hidden;
    hidden.name = "manual";
    hidden.disableModelInvocation = true;
    SkillPromptFormatter formatter;
    const std::string text = formatter.format({visible, hidden}, "read");
    EXPECT_EQ(text.rfind("\n\nThe following skills provide specialized instructions for specific tasks.\n", 0), 0U);
    EXPECT_NE(text.find("Use the read tool to load a skill's file"), std::string::npos);
    EXPECT_NE(text.find("<name>pdf</name>"), std::string::npos);
    EXPECT_NE(text.find("<description>Use &lt;pdf&gt; &amp; friends</description>"), std::string::npos);
    EXPECT_NE(text.find("<location>/s/pdf/SKILL.md</location>"), std::string::npos);
    EXPECT_EQ(text.find("manual"), std::string::npos);
    EXPECT_NE(text.find("</available_skills>"), std::string::npos);
    EXPECT_NE(formatter.format({visible}, "bash").find("Use bash to load"), std::string::npos);
}

TEST(SkillPromptFormatterTest, EmptyWhenNothingVisible) {
    SkillPromptFormatter formatter;
    Skill hidden;
    hidden.disableModelInvocation = true;
    EXPECT_EQ(formatter.format({}, "read"), "");
    EXPECT_EQ(formatter.format({hidden}, "read"), "");
}
