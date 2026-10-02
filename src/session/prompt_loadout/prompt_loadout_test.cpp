#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.session.prompt_loadout;
import pi.testing.fake_resource_loader;
import pi.testing.scripted_tool;
import pi.testing.session_harness;
import pi.tools.tool_registry;

class PromptLoadoutTest : public testing::Test {
protected:
    PromptLoadoutTest() : m_agent(m_harness.makeAgent()) {
        m_read = addTool("read", "Read file contents", {"Use read to examine files."});
        m_write = addTool("write", "Create or overwrite files", {});
        m_grep = addTool("grep", "Search file contents", {});
        LoadedResources resources;
        resources.systemPrompt = std::nullopt;
        resources.contextFiles.push_back(ContextFile{"/work/AGENTS.md", "Be careful."});
        m_resources.set(resources);
        m_loadout = std::make_unique<PromptLoadout>(*m_agent, m_harness.session(), m_registry, m_resources,
                                                    m_harness.clock(), "/work");
        m_loadout->setActiveTools({"read", "write"});
    }

    std::shared_ptr<ScriptedTool> addTool(const std::string& name, const std::string& snippet,
                                          std::vector<std::string> guidelines) {
        auto tool = std::make_shared<ScriptedTool>(name, "ok");
        tool->setPrompt(snippet, std::move(guidelines));
        m_registry.add(tool);
        return tool;
    }

    std::vector<std::string> agentToolNames() {
        std::vector<std::string> names;
        for (const auto& tool : m_agent->state().tools) {
            names.push_back(tool->definition().name);
        }
        return names;
    }

    SessionHarness m_harness;
    std::unique_ptr<IAgent> m_agent;
    ToolRegistry m_registry;
    FakeResourceLoader m_resources;
    std::shared_ptr<ScriptedTool> m_read;
    std::shared_ptr<ScriptedTool> m_write;
    std::shared_ptr<ScriptedTool> m_grep;
    std::unique_ptr<PromptLoadout> m_loadout;
};

TEST_F(PromptLoadoutTest, ActiveToolsUpdateRegistryAgentAndPromptOptions) {
    EXPECT_EQ(m_loadout->activeToolNames(), (std::vector<std::string>{"read", "write"}));
    EXPECT_EQ(agentToolNames(), (std::vector<std::string>{"read", "write"}));
    const BuildSystemPromptOptions options = m_loadout->baseOptions();
    EXPECT_EQ(options.selectedTools, (std::vector<std::string>{"read", "write"}));
    EXPECT_EQ(options.toolSnippets.at("read"), "Read file contents");
    EXPECT_EQ(options.toolGuidelines.at("read"), (std::vector<std::string>{"Use read to examine files."}));
    EXPECT_EQ(options.contextFiles.size(), 1U);
    EXPECT_EQ(options.cwd, "/work");
}

TEST_F(PromptLoadoutTest, UnknownAndFilteredToolsAreIgnored) {
    m_loadout->setToolFilter(std::nullopt, {"write"});
    m_loadout->setActiveTools({"read", "write", "nope", "read"});
    EXPECT_EQ(m_loadout->activeToolNames(), (std::vector<std::string>{"read"}));
    m_loadout->setToolFilter(std::set<std::string>{"grep"}, {});
    m_loadout->setActiveTools({"read", "grep"});
    EXPECT_EQ(m_loadout->activeToolNames(), (std::vector<std::string>{"grep"}));
}

TEST_F(PromptLoadoutTest, PrepareProducesSectionsOnceThenNothing) {
    BuildSystemPromptOptions options = m_loadout->baseOptions();
    const auto first = m_loadout->prepare(options, {});
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->sections.has_value());
    EXPECT_FALSE(first->sections->empty());
    EXPECT_EQ(first->timestamp, m_harness.clock().nowMs());
    const std::vector<AgentMessage> transcript = {AgentMessage(*first)};
    BuildSystemPromptOptions again = m_loadout->baseOptions();
    EXPECT_FALSE(m_loadout->prepare(again, transcript).has_value());
}

TEST_F(PromptLoadoutTest, ChangingToolsChangesOnlyTheToolsSection) {
    BuildSystemPromptOptions options = m_loadout->baseOptions();
    const auto first = m_loadout->prepare(options, {});
    const std::vector<AgentMessage> transcript = {AgentMessage(*first)};
    m_loadout->setActiveTools({"read", "write", "grep"});
    BuildSystemPromptOptions next = m_loadout->baseOptions();
    const auto update = m_loadout->prepare(next, transcript);
    ASSERT_TRUE(update.has_value());
    ASSERT_TRUE(update->sections.has_value());
    bool toolsChanged = false;
    for (const auto& [name, text] : *update->sections) {
        toolsChanged = toolsChanged || (name == "tools" && text.has_value() && text->find("grep") != std::string::npos);
    }
    EXPECT_TRUE(toolsChanged);
    EXPECT_LT(update->sections->size(), first->sections->size());
}

TEST_F(PromptLoadoutTest, SystemPromptTextMentionsActiveToolsAndContext) {
    const std::string text = m_loadout->systemPromptText();
    EXPECT_NE(text.find("read: Read file contents"), std::string::npos);
    EXPECT_NE(text.find("Be careful."), std::string::npos);
    EXPECT_EQ(text.find("grep: Search file contents"), std::string::npos);
}

TEST_F(PromptLoadoutTest, RestoreFromTranscriptReactivatesDeclaredTools) {
    SystemMessage declared;
    declared.content = std::string("prompt");
    declared.toolsAdded = std::vector<Tool>{m_grep->definition()};
    ASSERT_TRUE(m_harness.session().appendMessage(declared).has_value());
    m_loadout->restoreFromTranscript();
    EXPECT_EQ(m_loadout->activeToolNames(), (std::vector<std::string>{"grep"}));
}
