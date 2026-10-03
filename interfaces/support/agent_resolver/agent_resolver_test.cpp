#include <gtest/gtest.h>

import std;
import pi.support.agent_resolver;

class AgentResolverTest : public ::testing::Test {
protected:
    ToolRegistration tool(const std::string& name, const std::string& description = "") {
        ToolRegistration registration;
        registration.name = name;
        registration.description = description;
        return registration;
    }

    PromptSection section(const std::string& key, const std::string& text) {
        return PromptSection{key, [text](const PromptInput&) -> Result<std::optional<std::string>> { return std::optional<std::string>(text); }, true};
    }

    void install(Extension extension) {
        ASSERT_TRUE(m_registry.install(std::move(extension)).has_value());
    }

    std::vector<std::string> toolNames(const IAgent& agent) {
        std::vector<std::string> names;
        for (const ToolRegistration& registration : agent.snapshot()->tools) {
            names.push_back(registration.name);
        }
        return names;
    }

    std::shared_ptr<ResolvedAgent> resolve(const std::optional<Json>& state, const ResolvedSettings& settings = {}) {
        return m_resolver.resolve(state, *m_registry.snapshot(), settings, [this](const Error& error) { m_reports.push_back(error.message); });
    }

    void SetUp() override {
        Extension base;
        base.name = "base";
        base.tools = {tool("read"), tool("write")};
        base.sections = {section("env", "env text")};
        install(base);
        Extension extra;
        extra.name = "extra";
        extra.tools = {tool("search"), tool("write", "replaced")};
        extra.sections = {section("git", "git text")};
        install(extra);
    }

    Registry m_registry;
    AgentResolver m_resolver;
    std::vector<std::string> m_reports;
};

TEST_F(AgentResolverTest, AbsentStateSelectsEveryInstalledExtensionInOrder) {
    auto agent = resolve(std::nullopt);
    EXPECT_EQ(agent->snapshot()->extensionNames, std::vector<std::string>({"base", "extra"}));
    // A later extension's tool replaces an earlier one in place.
    EXPECT_EQ(toolNames(*agent), std::vector<std::string>({"read", "write", "search"}));
    EXPECT_EQ(agent->snapshot()->tools[1].description, "replaced");
    ASSERT_EQ(agent->sections().size(), 2u);
    EXPECT_EQ(agent->sections()[0].key, "env");
    EXPECT_EQ(agent->snapshot()->thinkingLevel, "off");
    EXPECT_FALSE(agent->snapshot()->model.has_value());
}

TEST_F(AgentResolverTest, StoredExtensionListSelectsExactlyThoseAndEditsAdjustTheDefault) {
    auto only = resolve(Json::parse(R"({"extensions":["extra"]})"));
    EXPECT_EQ(only->snapshot()->extensionNames, std::vector<std::string>({"extra"}));
    auto removed = resolve(Json::parse(R"({"extensions":{"remove":["base"]}})"));
    EXPECT_EQ(removed->snapshot()->extensionNames, std::vector<std::string>({"extra"}));
    ResolvedSettings settings;
    settings.extensions = std::vector<std::string>{"base"};
    auto added = resolve(Json::parse(R"({"extensions":{"add":["extra","missing"]}})"), settings);
    EXPECT_EQ(added->snapshot()->extensionNames, std::vector<std::string>({"base", "extra"}));
}

TEST_F(AgentResolverTest, ToolFilterListsOrRemoves) {
    EXPECT_EQ(toolNames(*resolve(Json::parse(R"({"tools":["search","read","read","nope"]})"))), std::vector<std::string>({"search", "read"}));
    EXPECT_EQ(toolNames(*resolve(Json::parse(R"({"tools":{"remove":["write"]}})"))), std::vector<std::string>({"read", "search"}));
}

TEST_F(AgentResolverTest, InstructionsBecomeTheLastSectionAndFieldsAreCarried) {
    auto agent = resolve(Json::parse(R"({"model":{"provider":"p","modelId":"m"},"thinkingLevel":"high","instructions":"be brief","cwd":"/w"})"));
    ASSERT_EQ(agent->sections().size(), 3u);
    EXPECT_EQ(agent->sections()[2].key, "instructions");
    auto rendered = agent->sections()[2].render(PromptInput{});
    ASSERT_TRUE(rendered.has_value());
    EXPECT_EQ(**rendered, "be brief");
    EXPECT_EQ(agent->snapshot()->thinkingLevel, "high");
    EXPECT_EQ(agent->snapshot()->model->at("modelId"), "m");
    EXPECT_EQ(*agent->snapshot()->cwd, "/w");
    EXPECT_EQ(*agent->snapshot()->instructions, "be brief");
}

TEST_F(AgentResolverTest, WrappersRewriteTargetsAndRenamingOnesDropThemWithAReport) {
    Extension wrapping;
    wrapping.name = "wrapping";
    Wrap good;
    good.kind = "tool";
    good.target = "read";
    good.wrapTool = [](const ToolRegistration& original) {
        ToolRegistration wrapped = original;
        wrapped.description = "wrapped";
        return wrapped;
    };
    Wrap renaming;
    renaming.kind = "tool";
    renaming.target = "search";
    renaming.wrapTool = [](const ToolRegistration& original) {
        ToolRegistration wrapped = original;
        wrapped.name = "other";
        return wrapped;
    };
    Wrap untargeted;
    untargeted.kind = "section";
    untargeted.target = "nothing";
    untargeted.wrapSection = [](const PromptSection& original) { return original; };
    wrapping.wraps = {good, renaming, untargeted};
    install(wrapping);
    auto agent = resolve(std::nullopt);
    EXPECT_EQ(toolNames(*agent), std::vector<std::string>({"read", "write"}));
    EXPECT_EQ(agent->snapshot()->tools[0].description, "wrapped");
    ASSERT_EQ(m_reports.size(), 1u);
    EXPECT_NE(m_reports[0].find("renamed search to other"), std::string::npos);
}
