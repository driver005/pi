#include <gtest/gtest.h>

import std;
import pi.support.prompt_template_loader;
import pi.testing.fake_file_system;

class PromptTemplateLoaderTest : public testing::Test {
protected:
    void file(const std::string& path, const std::string& content) {
        m_files.createDirectories(path.substr(0, path.find_last_of('/')));
        m_files.writeFile(path, content);
    }

    FakeFileSystem m_files;
    PromptTemplateLoader m_loader{m_files};
};

TEST_F(PromptTemplateLoaderTest, LoadsDefaultsWithScopesAndDescriptions) {
    file("/agent/prompts/review.md", "---\ndescription: Review code\nargument-hint: <file>\n---\nReview $1");
    file("/work/.pi/prompts/ship.md", "\n\nShip the thing now and tell me everything about what happened in great detail please\nmore");
    file("/work/.pi/prompts/readme.txt", "ignored");
    LoadPromptTemplatesOptions options;
    options.cwd = "/work";
    options.agentDir = "/agent";
    const auto result = m_loader.load(options);
    ASSERT_EQ(result.templates.size(), 2U);
    EXPECT_EQ(result.templates[0].name, "review");
    EXPECT_EQ(result.templates[0].description, "Review code");
    EXPECT_EQ(result.templates[0].argumentHint, "<file>");
    EXPECT_EQ(result.templates[0].content, "Review $1");
    EXPECT_EQ(result.templates[0].sourceInfo.scope, "user");
    EXPECT_EQ(result.templates[1].name, "ship");
    EXPECT_EQ(result.templates[1].sourceInfo.scope, "project");
    EXPECT_EQ(result.templates[1].description, "Ship the thing now and tell me everything about what happene...");
    EXPECT_FALSE(result.templates[1].argumentHint.has_value());
}

TEST_F(PromptTemplateLoaderTest, ExplicitPathsFilesAndDirectories) {
    file("/extra/one.md", "first line only");
    file("/extra/dir/two.md", "second");
    LoadPromptTemplatesOptions options;
    options.cwd = "/work";
    options.agentDir = "/agent";
    options.includeDefaults = false;
    options.promptPaths = {"/extra/one.md", "/extra/dir", "/missing.md", "/extra/dir/notmd.txt"};
    const auto result = m_loader.load(options);
    ASSERT_EQ(result.templates.size(), 2U);
    EXPECT_EQ(result.templates[0].name, "one");
    EXPECT_EQ(result.templates[0].description, "first line only");
    EXPECT_EQ(result.templates[0].sourceInfo.baseDir, "/extra");
    EXPECT_EQ(result.templates[1].name, "two");
    EXPECT_EQ(result.templates[1].sourceInfo.baseDir, "/extra/dir");
}

TEST_F(PromptTemplateLoaderTest, EmptyDirectoriesYieldNothing) {
    LoadPromptTemplatesOptions options;
    options.cwd = "/work";
    options.agentDir = "/agent";
    EXPECT_TRUE(m_loader.load(options).templates.empty());
}

TEST_F(PromptTemplateLoaderTest, ProjectTemplatesAreSkippedWhenNotIncluded) {
    file("/agent/prompts/a.md", "a");
    file("/work/.pi/prompts/b.md", "b");
    LoadPromptTemplatesOptions options;
    options.cwd = "/work";
    options.agentDir = "/agent";
    options.includeProject = false;
    const auto result = m_loader.load(options);
    ASSERT_EQ(result.templates.size(), 1U);
    EXPECT_EQ(result.templates[0].name, "a");
}
