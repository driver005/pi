#include <gtest/gtest.h>

import std;
import pi.support.pi_prompt_extension;
import pi.testing.scripted_tool;

class PiPromptExtensionTest : public ::testing::Test {
protected:
    PiPromptExtensionTest()
        : m_tools(std::make_shared<ToolSetCache>([](const std::string&) {
              auto tool = std::make_shared<ScriptedTool>("echo", "pong");
              tool->setPrompt("  Echo   things\n back ", {"Use echo wisely.", "Use echo wisely.", " "});
              return ToolSetCache::ToolSet{tool};
          })),
          m_resources(std::make_shared<ResourceSetCache>([this](const std::string& cwd) {
              m_loadedFor.push_back(cwd);
              LoadedResources resources;
              resources.contextFiles.push_back(ContextFile{cwd + "/AGENTS.md", "Be kind."});
              if (cwd == "/appended") {
                  resources.appendSystemPrompt = {"first", "second"};
              }
              return resources;
          })),
          m_extension(PiPromptExtension(m_tools, m_resources, "/default").extension()) {}

    std::optional<std::string> render(const std::string& key, const std::optional<std::string>& cwd, const std::vector<std::string>& tools) {
        auto agent = std::make_shared<AgentSnapshot>();
        agent->cwd = cwd;
        for (const std::string& name : tools) {
            ToolRegistration registration;
            registration.name = name;
            agent->tools.push_back(registration);
        }
        PromptInput input;
        input.agent = agent;
        for (const PromptSection& section : m_extension.sections) {
            if (section.key == key) {
                auto rendered = section.render(input);
                EXPECT_TRUE(rendered.has_value());
                return rendered ? *rendered : std::nullopt;
            }
        }
        ADD_FAILURE() << "no section " << key;
        return std::nullopt;
    }

    std::shared_ptr<ToolSetCache> m_tools;
    std::shared_ptr<ResourceSetCache> m_resources;
    std::vector<std::string> m_loadedFor;
    Extension m_extension;
};

TEST_F(PiPromptExtensionTest, OffersPiSectionsUntaggedBecauseTheyCarryTheirOwnTags) {
    std::vector<std::string> keys;
    for (const PromptSection& section : m_extension.sections) {
        keys.push_back(section.key);
        EXPECT_FALSE(section.tag) << section.key;
    }
    EXPECT_EQ(keys.front(), "preamble");
    EXPECT_EQ(keys.back(), "cwd");
    EXPECT_EQ(m_extension.name, "pi-prompt");
}

TEST_F(PiPromptExtensionTest, ListsTheOfferedToolsWithTheirSnippetsAndGuidelines) {
    const std::string tools = render("tools", std::nullopt, {"echo"}).value();
    EXPECT_NE(tools.find("- echo: Echo things back"), std::string::npos);
    const std::string rules = render("rules", std::nullopt, {"echo"}).value();
    EXPECT_NE(rules.find("Use echo wisely."), std::string::npos);
    // The duplicate guideline and the blank one are dropped.
    EXPECT_EQ(rules.find("Use echo wisely.", rules.find("Use echo wisely.") + 1), std::string::npos);
    EXPECT_NE(render("tools", std::nullopt, {}).value().find("(none)"), std::string::npos);
}

TEST_F(PiPromptExtensionTest, FollowsTheConversationsDirectory) {
    EXPECT_EQ(render("cwd", std::nullopt, {}).value(), "<cwd>\n/default\n</cwd>");
    EXPECT_EQ(render("cwd", std::string("/work/project"), {}).value(), "<cwd>\n/work/project\n</cwd>");
    EXPECT_NE(render("project_context", std::string("/work/project"), {}).value().find("Be kind."), std::string::npos);
    EXPECT_NE(std::find(m_loadedFor.begin(), m_loadedFor.end(), "/work/project"), m_loadedFor.end());
}

TEST_F(PiPromptExtensionTest, SectionsWithoutContentAreOmittedAndAppendedTextIsAnAddendum) {
    EXPECT_FALSE(render("skills", std::nullopt, {"echo"}).has_value());
    EXPECT_FALSE(render("docs", std::nullopt, {"echo"}).has_value());
    EXPECT_FALSE(render("addendum", std::nullopt, {}).has_value());
    EXPECT_EQ(render("addendum", std::string("/appended"), {}).value(), "<addendum>\nfirst\n\nsecond\n</addendum>");
}
