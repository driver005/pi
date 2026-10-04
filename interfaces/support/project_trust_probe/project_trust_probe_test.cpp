#include <gtest/gtest.h>

import std;
import pi.support.project_trust_probe;
import pi.testing.fake_file_system;

class ProjectTrustProbeTest : public testing::Test {
protected:
    void touch(const std::string& path) {
        m_files.createDirectories(path.substr(0, path.find_last_of('/')));
        m_files.writeFile(path, "x");
    }

    FakeFileSystem m_files{"/home/user"};
    ProjectTrustProbe m_probe{m_files};
};

TEST_F(ProjectTrustProbeTest, PlainProjectNeedsNoTrust) {
    m_files.createDirectories("/work/p");
    EXPECT_FALSE(m_probe.requiresTrust("/work/p"));
}

TEST_F(ProjectTrustProbeTest, ConfigResourcesRequireTrust) {
    for (const char* entry : {"settings.json", "mcp.json", "SYSTEM.md", "APPEND_SYSTEM.md"}) {
        FakeFileSystem files{"/home/user"};
        ProjectTrustProbe probe(files);
        files.createDirectories("/work/p/.pi");
        files.writeFile(std::string("/work/p/.pi/") + entry, "x");
        EXPECT_TRUE(probe.requiresTrust("/work/p")) << entry;
    }
    m_files.createDirectories("/work/q/.pi/extensions");
    EXPECT_TRUE(m_probe.requiresTrust("/work/q"));
    m_files.createDirectories("/work/r/.pi/plugins");
    EXPECT_TRUE(m_probe.requiresTrust("/work/r"));
}

TEST_F(ProjectTrustProbeTest, AgentsSkillsInAncestorRequireTrustButUserOnesDoNot) {
    m_files.createDirectories("/work/.agents/skills");
    m_files.createDirectories("/work/p/src");
    EXPECT_TRUE(m_probe.requiresTrust("/work/p/src"));
    m_files.createDirectories("/home/user/.agents/skills");
    m_files.createDirectories("/home/user/proj");
    EXPECT_FALSE(m_probe.requiresTrust("/home/user/proj"));
}

TEST_F(ProjectTrustProbeTest, ConfigDirOfAnAncestorDoesNotCount) {
    touch("/work/.pi/settings.json");
    m_files.createDirectories("/work/p");
    EXPECT_FALSE(m_probe.requiresTrust("/work/p"));
}
