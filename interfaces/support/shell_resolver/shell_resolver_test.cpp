#include <gtest/gtest.h>

import std;
import pi.support.shell_resolver;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;

class ShellResolverTest : public testing::Test {
protected:
    void touch(const std::string& path) {
        m_files.createDirectories(path.substr(0, path.rfind('/')));
        m_files.writeFile(path, "");
    }

    FakeFileSystem m_files;
    FakeEnvironment m_environment;
    ShellResolver m_resolver{m_files, m_environment};
};

TEST_F(ShellResolverTest, ConfiguredPathWinsWhenItExists) {
    touch("/opt/zsh");
    touch("/bin/bash");
    EXPECT_EQ(m_resolver.resolve("/opt/zsh"), "/opt/zsh");
    EXPECT_EQ(m_resolver.resolve("/missing/zsh"), "/bin/bash");
}

TEST_F(ShellResolverTest, FallsBackToBashOnPathThenSh) {
    EXPECT_EQ(m_resolver.resolve(""), "sh");
    touch("/usr/local/bin/bash");
    m_environment.set("PATH", "/nowhere:/usr/local/bin");
    EXPECT_EQ(m_resolver.resolve(""), "/usr/local/bin/bash");
}
