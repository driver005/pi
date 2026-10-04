#include <gtest/gtest.h>

import std;
import pi.support.system_prompt_builder;

class SystemPromptBuilderTest : public testing::Test {
protected:
    BuildSystemPromptOptions options() {
        BuildSystemPromptOptions o;
        o.cwd = "/work/project";
        o.toolSnippets = {{"read", "Read file contents"}, {"bash", "Execute bash commands"}};
        return o;
    }

    std::string section(const SystemPromptBuilder::Sections& sections, const std::string& name) {
        for (const auto& entry : sections) {
            if (entry.first == name) {
                return entry.second;
            }
        }
        return "<missing>";
    }

    SystemPromptBuilder m_builder;
};

TEST_F(SystemPromptBuilderTest, DefaultSectionsInOrder) {
    const auto sections = m_builder.buildSections(options());
    ASSERT_TRUE(sections.has_value());
    std::vector<std::string> names;
    for (const auto& entry : *sections) {
        names.push_back(entry.first);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"preamble", "tools", "rules", "cwd"}));
    EXPECT_EQ(section(*sections, "preamble").rfind("You are an expert coding assistant", 0), 0U);
    EXPECT_EQ(section(*sections, "tools").rfind("<tools>\n- read: Read file contents\n- bash: Execute bash commands", 0), 0U);
    EXPECT_EQ(section(*sections, "cwd"), "<cwd>\n/work/project\n</cwd>");
}

TEST_F(SystemPromptBuilderTest, RulesDeduplicateAndAdaptToTools) {
    BuildSystemPromptOptions o = options();
    o.promptGuidelines = {"Be concise in your responses", "Prefer small diffs"};
    o.toolGuidelines = {{"read", {"Read before editing"}}};
    const auto rules = section(*m_builder.buildSections(o), "rules");
    EXPECT_EQ(rules,
              "<rules>\n- Use bash for file operations like ls, rg, find\n- Read before editing\n- Be concise in your "
              "responses\n- Prefer small diffs\n- Show file paths clearly when working with files\n</rules>");
    o.selectedTools = {"read", "bash", "grep"};
    EXPECT_EQ(section(*m_builder.buildSections(o), "rules").find("Use bash for file operations"), std::string::npos);
}

TEST_F(SystemPromptBuilderTest, CustomPromptReplacesDefaultsAndKeepsOtherSections) {
    BuildSystemPromptOptions o = options();
    o.customPrompt = "You are a pirate.";
    o.appendSystemPrompt = "Always say arr.";
    o.contextFiles = {ContextFile{"/work/AGENTS.md", "Use tabs."}};
    const auto sections = *m_builder.buildSections(o);
    EXPECT_EQ(section(sections, "preamble"), "You are a pirate.");
    EXPECT_EQ(section(sections, "tools"), "<missing>");
    EXPECT_EQ(section(sections, "addendum"), "<addendum>\nAlways say arr.\n</addendum>");
    EXPECT_EQ(section(sections, "project_context"),
              "<project_context>\nProject-specific instructions and guidelines:\n\n<project_instructions "
              "path=\"/work/AGENTS.md\">\nUse tabs.\n</project_instructions>\n</project_context>");
}

TEST_F(SystemPromptBuilderTest, DocsSectionOnlyWithDocsPaths) {
    BuildSystemPromptOptions o = options();
    EXPECT_EQ(section(*m_builder.buildSections(o), "docs"), "<missing>");
    o.readmePath = "/pi/README.md";
    o.docsPath = "/pi/docs";
    o.examplesPath = "/pi/examples";
    const std::string docs = section(*m_builder.buildSections(o), "docs");
    EXPECT_NE(docs.find("- Main documentation: /pi/README.md"), std::string::npos);
    EXPECT_NE(docs.find("- Examples: /pi/examples (extensions, custom tools, SDK)"), std::string::npos);
}

TEST_F(SystemPromptBuilderTest, SkillsAppearWhenAFileReadToolExists) {
    BuildSystemPromptOptions o = options();
    Skill skill;
    skill.name = "pdf";
    skill.description = "PDFs";
    skill.filePath = "/s/pdf/SKILL.md";
    o.skills = {skill};
    EXPECT_NE(section(*m_builder.buildSections(o), "skills").find("<name>pdf</name>"), std::string::npos);
    o.selectedTools = {"write"};
    EXPECT_EQ(section(*m_builder.buildSections(o), "skills"), "<missing>");
}

TEST_F(SystemPromptBuilderTest, CustomSectionsAreValidatedAndAppended) {
    BuildSystemPromptOptions o = options();
    o.sections = {{"my-notes", "remember"}, {"empty", ""}};
    const auto sections = *m_builder.buildSections(o);
    EXPECT_EQ(section(sections, "my-notes"), "<my-notes>\nremember\n</my-notes>");
    EXPECT_EQ(section(sections, "empty"), "<missing>");
    o.sections = {{"Bad", "x"}};
    EXPECT_FALSE(m_builder.buildSections(o).has_value());
    o.sections = {{"preamble", "x"}};
    EXPECT_EQ(m_builder.buildSections(o).error().message, "Invalid system prompt section name: preamble");
}

TEST_F(SystemPromptBuilderTest, ForcedPromptIsOpaque) {
    BuildSystemPromptOptions o = options();
    o.forceSystemPrompt = "exactly this";
    const auto state = m_builder.buildState(o);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->content, "exactly this");
    EXPECT_FALSE(state->sections.has_value());
    EXPECT_EQ(*m_builder.buildText(o), "exactly this");
}

TEST_F(SystemPromptBuilderTest, TextRendersSectionsFromTheSystemMessage) {
    const std::string text = *m_builder.buildText(options());
    EXPECT_NE(text.find("You are an expert coding assistant"), std::string::npos);
    EXPECT_NE(text.find("<tools>\n- read: Read file contents"), std::string::npos);
    EXPECT_NE(text.find("<cwd>\n/work/project\n</cwd>"), std::string::npos);
}

TEST_F(SystemPromptBuilderTest, DiffReportsChangedAndRemovedSections) {
    const SystemPromptBuilder::Sections previous = {{"preamble", "a"}, {"tools", "t1"}, {"cwd", "c"}};
    const SystemPromptBuilder::Sections current = {{"preamble", "a"}, {"tools", "t2"}, {"rules", "r"}};
    const auto patch = m_builder.diff(previous, current);
    ASSERT_EQ(patch.size(), 3U);
    EXPECT_EQ(patch[0].first, "tools");
    EXPECT_EQ(patch[0].second, "t2");
    EXPECT_EQ(patch[1].first, "rules");
    EXPECT_EQ(patch[2].first, "cwd");
    EXPECT_FALSE(patch[2].second.has_value());
    EXPECT_TRUE(m_builder.diff(current, current).empty());
}
