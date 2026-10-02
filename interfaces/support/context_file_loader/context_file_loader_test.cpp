#include <gtest/gtest.h>

import std;
import pi.support.context_file_loader;
import pi.testing.fake_file_system;

class ContextFileLoaderTest : public testing::Test {
protected:
    void file(const std::string& path, const std::string& content) {
        m_files.createDirectories(path.substr(0, path.find_last_of('/')));
        m_files.writeFile(path, content);
    }

    FakeFileSystem m_files;
    ContextFileLoader m_loader{m_files};
};

TEST_F(ContextFileLoaderTest, GlobalFirstThenAncestorsOutermostToInnermost) {
    file("/agent/AGENTS.md", "global");
    file("/repo/AGENTS.md", "repo");
    file("/repo/pkg/CLAUDE.md", "pkg");
    file("/repo/pkg/src/AGENTS.md", "src");
    const auto files = m_loader.load("/repo/pkg/src", "/agent");
    ASSERT_EQ(files.size(), 4U);
    EXPECT_EQ(files[0].content, "global");
    EXPECT_EQ(files[1].path, "/repo/AGENTS.md");
    EXPECT_EQ(files[2].path, "/repo/pkg/CLAUDE.md");
    EXPECT_EQ(files[3].path, "/repo/pkg/src/AGENTS.md");
}

TEST_F(ContextFileLoaderTest, OverrideWinsAndOnlyOneFilePerDirectory) {
    file("/repo/AGENTS.override.md", "override");
    file("/repo/AGENTS.md", "plain");
    file("/repo/CLAUDE.md", "claude");
    const auto files = m_loader.load("/repo", "/agent");
    ASSERT_EQ(files.size(), 1U);
    EXPECT_EQ(files[0].content, "override");
}

TEST_F(ContextFileLoaderTest, DuplicateWhenCwdIsAgentDirIsLoadedOnce) {
    file("/agent/AGENTS.md", "g");
    const auto files = m_loader.load("/agent", "/agent");
    EXPECT_EQ(files.size(), 1U);
}

TEST_F(ContextFileLoaderTest, BomIsStrippedAndDirectoriesNamedAgentsIgnored) {
    file("/repo/AGENTS.md", "\xEF\xBB\xBFhello");
    m_files.createDirectories("/repo/sub/AGENTS.md");
    const auto files = m_loader.load("/repo/sub", "/agent");
    ASSERT_EQ(files.size(), 1U);
    EXPECT_EQ(files[0].content, "hello");
}

TEST_F(ContextFileLoaderTest, RootDirectoryContextFile) {
    file("/AGENTS.md", "root");
    const auto files = m_loader.load("/work", "/agent");
    ASSERT_EQ(files.size(), 1U);
    EXPECT_EQ(files[0].path, "/AGENTS.md");
}
