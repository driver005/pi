#include <gtest/gtest.h>

import std;
import pi.support.git_source_parser;

class GitSourceParserTest : public testing::Test {
protected:
    GitSourceParser m_parser;
};

TEST_F(GitSourceParserTest, PrefixedHostPathAndRef) {
    const auto source = m_parser.parse("git:github.com/user/repo@v1.2");
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->type, "git");
    EXPECT_EQ(source->host, "github.com");
    EXPECT_EQ(source->path, "user/repo");
    EXPECT_EQ(source->repo, "https://github.com/user/repo");
    EXPECT_EQ(source->ref, "v1.2");
    EXPECT_TRUE(source->pinned);
}

TEST_F(GitSourceParserTest, UserRepoIsGithub) {
    const auto source = m_parser.parse("git:user/repo");
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->host, "github.com");
    EXPECT_EQ(source->path, "user/repo");
    EXPECT_FALSE(source->ref.has_value());
    EXPECT_FALSE(source->pinned);
}

TEST_F(GitSourceParserTest, ScpLike) {
    const auto source = m_parser.parse("git@gitlab.com:group/sub/repo.git@main");
    EXPECT_FALSE(source.has_value()) << "scp-like needs the git: prefix";
    const auto prefixed = m_parser.parse("git:git@gitlab.com:group/sub/repo.git@main");
    ASSERT_TRUE(prefixed.has_value());
    EXPECT_EQ(prefixed->host, "gitlab.com");
    EXPECT_EQ(prefixed->path, "group/sub/repo");
    EXPECT_EQ(prefixed->repo, "git@gitlab.com:group/sub/repo.git");
    EXPECT_EQ(prefixed->ref, "main");
}

TEST_F(GitSourceParserTest, ProtocolUrlsNeedNoPrefix) {
    const auto https = m_parser.parse("https://Example.com/user/repo.git");
    ASSERT_TRUE(https.has_value());
    EXPECT_EQ(https->host, "example.com");
    EXPECT_EQ(https->path, "user/repo");
    const auto ssh = m_parser.parse("ssh://git@host.org:2222/team/tool@abc123");
    ASSERT_TRUE(ssh.has_value());
    EXPECT_EQ(ssh->host, "host.org");
    EXPECT_EQ(ssh->path, "team/tool");
    EXPECT_EQ(ssh->ref, "abc123");
}

TEST_F(GitSourceParserTest, RejectsNonGitAndUnsafeSources) {
    EXPECT_FALSE(m_parser.parse("./local/dir").has_value());
    EXPECT_FALSE(m_parser.parse("user/repo").has_value());
    EXPECT_FALSE(m_parser.parse("git:github.com/../etc").has_value());
    EXPECT_FALSE(m_parser.parse("git:github.com/user/%2e%2e/repo").has_value());
    EXPECT_FALSE(m_parser.parse("git:github.com/user\\repo").has_value());
    EXPECT_FALSE(m_parser.parse("git:github.com/%zz/repo").has_value());
    EXPECT_FALSE(m_parser.parse("git:github.com/user%").has_value());
    EXPECT_FALSE(m_parser.parse("git:github.com/onlyone").has_value());
}
