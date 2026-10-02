#include <gtest/gtest.h>

import std;
import pi.support.skill_loader;
import pi.testing.fake_file_system;

class SkillLoaderTest : public testing::Test {
protected:
    void skill(const std::string& path, const std::string& name, const std::string& description,
               const std::string& extra = "") {
        const auto slash = path.find_last_of('/');
        m_files.createDirectories(path.substr(0, slash));
        m_files.writeFile(path, "---\nname: " + name + "\ndescription: " + description + "\n" + extra + "---\n# Body\n");
    }

    FakeFileSystem m_files;
    SkillLoader m_loader{m_files};
};

TEST_F(SkillLoaderTest, SkillMdIsARootAndNestedFilesAreNotLoaded) {
    skill("/s/pdf/SKILL.md", "pdf", "Work with PDFs");
    skill("/s/pdf/nested/SKILL.md", "nested", "Never loaded");
    const auto result = m_loader.loadFromDir("/s", "user");
    ASSERT_EQ(result.skills.size(), 1U);
    EXPECT_EQ(result.skills[0].name, "pdf");
    EXPECT_EQ(result.skills[0].baseDir, "/s/pdf");
    EXPECT_EQ(result.skills[0].sourceInfo.scope, "user");
    EXPECT_FALSE(result.skills[0].disableModelInvocation);
}

TEST_F(SkillLoaderTest, DirectMdFilesInRootNeedADescription) {
    m_files.createDirectories("/s");
    m_files.writeFile("/s/quick.md", "---\ndescription: Quick helper\n---\nbody");
    m_files.writeFile("/s/notes.md", "# just notes\n");
    const auto result = m_loader.loadFromDir("/s", "path");
    ASSERT_EQ(result.skills.size(), 1U);
    EXPECT_EQ(result.skills[0].name, "s");
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST_F(SkillLoaderTest, NameAndDescriptionValidationWarnsButLoads) {
    skill("/s/Bad_Name/SKILL.md", "Bad_Name", "ok");
    const auto result = m_loader.loadFromDir("/s", "user");
    ASSERT_EQ(result.skills.size(), 1U);
    ASSERT_EQ(result.diagnostics.size(), 1U);
    EXPECT_NE(result.diagnostics[0].message.find("invalid characters"), std::string::npos);
    EXPECT_EQ(m_loader.validateName("a--b").size(), 1U);
    EXPECT_EQ(m_loader.validateName("-a").size(), 1U);
    EXPECT_TRUE(m_loader.validateName("good-name-1").empty());
    EXPECT_EQ(m_loader.validateName(std::string(70, 'a')).size(), 1U);
}

TEST_F(SkillLoaderTest, SkillMdWithoutDescriptionIsSkippedWithWarning) {
    m_files.createDirectories("/s/x");
    m_files.writeFile("/s/x/SKILL.md", "---\nname: x\n---\nbody");
    const auto result = m_loader.loadFromDir("/s", "user");
    EXPECT_TRUE(result.skills.empty());
    ASSERT_EQ(result.diagnostics.size(), 1U);
    EXPECT_EQ(result.diagnostics[0].message, "description is required");
}

TEST_F(SkillLoaderTest, DisableModelInvocationFlag) {
    skill("/s/hidden/SKILL.md", "hidden", "Manual only", "disable-model-invocation: true\n");
    EXPECT_TRUE(m_loader.loadFromDir("/s", "user").skills[0].disableModelInvocation);
}

TEST_F(SkillLoaderTest, IgnoreFilesAndDotAndNodeModulesAreSkipped) {
    skill("/s/a/SKILL.md", "a", "A");
    skill("/s/ignored/SKILL.md", "ignored", "I");
    skill("/s/.hidden/SKILL.md", "hidden", "H");
    skill("/s/node_modules/dep/SKILL.md", "dep", "D");
    m_files.writeFile("/s/.gitignore", "ignored/\n");
    const auto result = m_loader.loadFromDir("/s", "user");
    ASSERT_EQ(result.skills.size(), 1U);
    EXPECT_EQ(result.skills[0].name, "a");
}

TEST_F(SkillLoaderTest, LoadMergesDefaultsAndExplicitPathsReportingCollisions) {
    skill("/home/user/.pi/agent/skills/shared/SKILL.md", "shared", "User version");
    skill("/work/.pi/skills/shared/SKILL.md", "shared", "Project version");
    skill("/work/.pi/skills/only-project/SKILL.md", "only-project", "P");
    skill("/extra/more/SKILL.md", "more", "Extra");
    LoadSkillsOptions options;
    options.cwd = "/work";
    options.agentDir = "/home/user/.pi/agent";
    options.skillPaths = {"/extra", "/missing", " /extra/more/SKILL.md "};
    const auto result = m_loader.load(options);
    ASSERT_EQ(result.skills.size(), 3U);
    EXPECT_EQ(result.skills[0].description, "User version");
    EXPECT_EQ(result.skills[0].sourceInfo.scope, "user");
    EXPECT_EQ(result.skills[2].sourceInfo.source, "local");
    int collisions = 0;
    int missing = 0;
    for (const auto& diagnostic : result.diagnostics) {
        collisions += diagnostic.type == "collision";
        missing += diagnostic.message == "skill path does not exist";
    }
    EXPECT_EQ(collisions, 1);
    EXPECT_EQ(missing, 1);
    EXPECT_EQ(result.diagnostics.back().collision->winnerPath, "/home/user/.pi/agent/skills/shared/SKILL.md");
}

TEST_F(SkillLoaderTest, WithoutDefaultsPathsUnderSkillDirsKeepTheirScope) {
    skill("/work/.pi/skills/p/SKILL.md", "p", "P");
    LoadSkillsOptions options;
    options.cwd = "/work";
    options.agentDir = "/home/user/.pi/agent";
    options.includeDefaults = false;
    options.skillPaths = {"/work/.pi/skills"};
    const auto result = m_loader.load(options);
    ASSERT_EQ(result.skills.size(), 1U);
    EXPECT_EQ(result.skills[0].sourceInfo.scope, "project");
}

TEST_F(SkillLoaderTest, ProjectSkillsAreSkippedWhenNotIncluded) {
    skill("/home/a/skills/user/SKILL.md", "usr", "User skill");
    skill("/work/.pi/skills/proj/SKILL.md", "proj", "Project skill");
    LoadSkillsOptions options;
    options.cwd = "/work";
    options.agentDir = "/home/a";
    EXPECT_EQ(m_loader.load(options).skills.size(), 2U);
    options.includeProject = false;
    const auto result = m_loader.load(options);
    ASSERT_EQ(result.skills.size(), 1U);
    EXPECT_EQ(result.skills[0].name, "usr");
}
