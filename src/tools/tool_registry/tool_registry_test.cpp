#include <gtest/gtest.h>

import std;
import pi.testing.scripted_tool;
import pi.tools.tool_registry;

class ToolRegistryTest : public testing::Test {
protected:
    std::vector<std::string> names(const std::vector<std::shared_ptr<ITool>>& tools) {
        std::vector<std::string> out;
        for (const auto& tool : tools) {
            out.push_back(tool->definition().name);
        }
        return out;
    }

    ToolRegistry m_registry;
};

TEST_F(ToolRegistryTest, AddFindAndOrder) {
    m_registry.add(std::make_shared<ScriptedTool>("b", "x"));
    m_registry.add(std::make_shared<ScriptedTool>("a", "y"));
    EXPECT_EQ(names(m_registry.all()), (std::vector<std::string>{"b", "a"}));
    EXPECT_NE(m_registry.find("a"), nullptr);
    EXPECT_EQ(m_registry.find("zzz"), nullptr);
}

TEST_F(ToolRegistryTest, ReplaceKeepsPositionAndRemoveWorks) {
    auto first = std::make_shared<ScriptedTool>("a", "1");
    auto second = std::make_shared<ScriptedTool>("a", "2");
    m_registry.add(first);
    m_registry.add(std::make_shared<ScriptedTool>("b", "x"));
    m_registry.add(second);
    EXPECT_EQ(m_registry.all().size(), 2U);
    EXPECT_EQ(m_registry.find("a"), second);
    EXPECT_TRUE(m_registry.remove("a"));
    EXPECT_FALSE(m_registry.remove("a"));
    EXPECT_EQ(names(m_registry.all()), (std::vector<std::string>{"b"}));
}

TEST_F(ToolRegistryTest, ActiveSubset) {
    for (const char* name : {"read", "bash", "edit"}) {
        m_registry.add(std::make_shared<ScriptedTool>(name, "x"));
    }
    EXPECT_EQ(names(m_registry.active()).size(), 3U);
    EXPECT_EQ(m_registry.setActive({"edit", "read", "missing"}), 2U);
    EXPECT_EQ(names(m_registry.active()), (std::vector<std::string>{"read", "edit"}));
    m_registry.add(std::make_shared<ScriptedTool>("late", "x"));
    EXPECT_EQ(names(m_registry.active()), (std::vector<std::string>{"read", "edit", "late"}));
}
