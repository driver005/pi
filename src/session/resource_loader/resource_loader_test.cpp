#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.session.resource_loader;
import pi.testing.fake_file_system;
import pi.testing.fake_settings_manager;

class ResourceLoaderTest : public testing::Test {
protected:
    void file(const std::string& path, const std::string& content) {
        m_files.createDirectories(path.substr(0, path.find_last_of('/')));
        m_files.writeFile(path, content);
    }

    ResourceLoaderOptions options() {
        ResourceLoaderOptions out;
        out.cwd = "/work";
        out.agentDir = "/agent";
        return out;
    }

    FakeFileSystem m_files;
    FakeSettingsManager m_settings;
};

TEST_F(ResourceLoaderTest, LoadsEverythingFromTheStandardLocations) {
    file("/agent/AGENTS.md", "global rules");
    file("/work/AGENTS.md", "project rules");
    file("/agent/skills/pdf/SKILL.md", "---\nname: pdf\ndescription: PDFs\n---\nbody");
    file("/work/.pi/prompts/ship.md", "Ship $1");
    file("/work/.pi/SYSTEM.md", "\xEF\xBB\xBFYou are a pirate.");
    file("/work/.pi/APPEND_SYSTEM.md", "Always say arr.");
    ResourceLoader loader(options(), m_settings, m_files);
    ASSERT_TRUE(loader.reload().has_value());
    const LoadedResources resources = loader.resources();
    ASSERT_EQ(resources.contextFiles.size(), 2U);
    EXPECT_EQ(resources.contextFiles[0].content, "global rules");
    ASSERT_EQ(resources.skills.size(), 1U);
    EXPECT_EQ(resources.skills[0].name, "pdf");
    ASSERT_EQ(resources.promptTemplates.size(), 1U);
    EXPECT_EQ(resources.promptTemplates[0].name, "ship");
    EXPECT_EQ(resources.systemPrompt, "You are a pirate.");
    EXPECT_EQ(resources.appendSystemPrompt, (std::vector<std::string>{"Always say arr."}));
}

TEST_F(ResourceLoaderTest, UntrustedProjectContributesNoProjectResources) {
    file("/agent/SYSTEM.md", "global system");
    file("/work/.pi/SYSTEM.md", "project system");
    file("/work/.pi/prompts/ship.md", "Ship");
    file("/work/.pi/skills/s/SKILL.md", "---\nname: s\ndescription: d\n---\nb");
    m_settings.setProjectTrusted(false);
    ResourceLoader loader(options(), m_settings, m_files);
    ASSERT_TRUE(loader.reload().has_value());
    const LoadedResources resources = loader.resources();
    EXPECT_EQ(resources.systemPrompt, "global system");
    EXPECT_TRUE(resources.promptTemplates.empty());
    EXPECT_TRUE(resources.skills.empty());
}

TEST_F(ResourceLoaderTest, ExplicitSourcesAreFilesOrLiteralText) {
    file("/prompts/system.txt", "from a file");
    ResourceLoaderOptions custom = options();
    custom.systemPrompt = "/prompts/system.txt";
    custom.appendSystemPrompt = std::vector<std::string>{"literal addendum", "/prompts/system.txt"};
    ResourceLoader loader(custom, m_settings, m_files);
    ASSERT_TRUE(loader.reload().has_value());
    const LoadedResources resources = loader.resources();
    EXPECT_EQ(resources.systemPrompt, "from a file");
    EXPECT_EQ(resources.appendSystemPrompt, (std::vector<std::string>{"literal addendum", "from a file"}));
}

TEST_F(ResourceLoaderTest, SwitchesCanTurnResourceKindsOff) {
    file("/work/AGENTS.md", "rules");
    file("/agent/skills/pdf/SKILL.md", "---\nname: pdf\ndescription: PDFs\n---\nbody");
    file("/agent/prompts/p.md", "p");
    ResourceLoaderOptions off = options();
    off.noContextFiles = true;
    off.noSkills = true;
    off.noPromptTemplates = true;
    ResourceLoader loader(off, m_settings, m_files);
    ASSERT_TRUE(loader.reload().has_value());
    const LoadedResources resources = loader.resources();
    EXPECT_TRUE(resources.contextFiles.empty());
    EXPECT_TRUE(resources.skills.empty());
    EXPECT_TRUE(resources.promptTemplates.empty());
}

TEST_F(ResourceLoaderTest, SettingsPathsAndAdditionalPathsAreLoaded) {
    file("/extra/skills/tool/SKILL.md", "---\nname: tool\ndescription: T\n---\nb");
    file("/more/hello.md", "Hello there");
    m_settings.setGlobal("skills", Json::array({"/extra/skills"}));
    m_settings.setGlobal("prompts", Json::array({"/more/hello.md"}));
    ResourceLoader loader(options(), m_settings, m_files);
    ASSERT_TRUE(loader.reload().has_value());
    const LoadedResources resources = loader.resources();
    ASSERT_EQ(resources.skills.size(), 1U);
    EXPECT_EQ(resources.skills[0].name, "tool");
    ASSERT_EQ(resources.promptTemplates.size(), 1U);
    EXPECT_EQ(resources.promptTemplates[0].name, "hello");
}

TEST_F(ResourceLoaderTest, ResourcesAreEmptyBeforeTheFirstReload) {
    ResourceLoader loader(options(), m_settings, m_files);
    EXPECT_TRUE(loader.resources().contextFiles.empty());
    EXPECT_FALSE(loader.resources().systemPrompt.has_value());
}
