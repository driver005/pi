#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.boring_crypto;
import pi.support.mcp_oauth_store;
import pi.testing.fake_file_system;

class RecordingLock : public IFileLock {
public:
    Result<void> withLock(const std::string& path, const std::function<Result<void>()>& action) override {
        m_paths.push_back(path);
        return action();
    }

    std::vector<std::string> m_paths;
};

class McpOauthStoreTest : public testing::Test {
protected:
    Json state(const std::string& url, const std::string& token) {
        return Json{{"serverUrl", url}, {"tokens", Json{{"access_token", token}, {"token_type", "Bearer"}}}};
    }

    FakeFileSystem m_files;
    RecordingLock m_lock;
    BoringCrypto m_crypto;
    McpOauthStore m_store{"/agent/mcp-auth.json", "/agent", m_files, m_lock, m_crypto};
};

TEST_F(McpOauthStoreTest, NothingIsStoredAtFirst) {
    const auto loaded = m_store.load("docs", "https://mcp.example.com/mcp");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_FALSE(loaded->has_value());
}

TEST_F(McpOauthStoreTest, StateRoundTripsUnderTheTypeScriptKey) {
    ASSERT_TRUE(m_store.save("my-docs", "https://MCP.example.com:443/mcp", state("https://mcp.example.com/mcp", "t1")).has_value());
    const Json file = Json::parse(*m_files.readFile("/agent/mcp-auth.json"));
    ASSERT_TRUE(file.contains("mcp__my_docs|https://mcp.example.com/mcp"));
    const auto loaded = m_store.load("my-docs", "https://mcp.example.com/mcp");
    ASSERT_TRUE(loaded.has_value() && loaded->has_value());
    EXPECT_EQ((**loaded)["tokens"]["access_token"], "t1");
    EXPECT_EQ(m_lock.m_paths.front(), "/agent/mcp-auth.json");
}

TEST_F(McpOauthStoreTest, TheFileIsPrettyPrintedWithATrailingNewline) {
    ASSERT_TRUE(m_store.save("docs", "https://mcp.example.com/", state("https://mcp.example.com/", "t")).has_value());
    const std::string text = *m_files.readFile("/agent/mcp-auth.json");
    EXPECT_EQ(text.back(), '\n');
    EXPECT_NE(text.find("\n  \"mcp__docs|https://mcp.example.com/\""), std::string::npos);
}

TEST_F(McpOauthStoreTest, StateForAnotherUrlUnderTheSameKeyIsIgnored) {
    ASSERT_TRUE(m_store.save("docs", "https://a.example.com/", state("https://evil.example.com/", "t")).has_value());
    const auto loaded = m_store.load("docs", "https://a.example.com/");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_FALSE(loaded->has_value());
}

TEST_F(McpOauthStoreTest, TheFirstServerTakesOverOldKeyState) {
    const std::string url = "https://mcp.example.com/mcp";
    ASSERT_TRUE(m_files.createDirectories("/agent").has_value());
    ASSERT_TRUE(m_files.writeFile("/agent/mcp-auth.json", Json{{url, state(url, "old")}}.dump()).has_value());
    const auto first = m_store.load("one", url);
    ASSERT_TRUE(first.has_value() && first->has_value());
    EXPECT_EQ((**first)["tokens"]["access_token"], "old");
    const auto second = m_store.load("two", url);
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second->has_value());
    const Json file = Json::parse(*m_files.readFile("/agent/mcp-auth.json"));
    EXPECT_FALSE(file.contains(url));
    EXPECT_TRUE(file.contains("mcp__one|" + url));
}

TEST_F(McpOauthStoreTest, RemoveDropsTheServersStateOnly) {
    const std::string url = "https://mcp.example.com/mcp";
    ASSERT_TRUE(m_store.save("one", url, state(url, "1")).has_value());
    ASSERT_TRUE(m_store.save("two", url, state(url, "2")).has_value());
    const auto removed = m_store.remove("one", url);
    ASSERT_TRUE(removed.has_value());
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(*m_store.remove("one", url));
    EXPECT_TRUE((*m_store.load("two", url)).has_value());
}

TEST_F(McpOauthStoreTest, RefreshLocksAreFilesPerServerInTheLockDirectory) {
    int ran = 0;
    ASSERT_TRUE(m_store.withRefreshLock("docs", "https://mcp.example.com/mcp", [&]() -> Result<void> {
        ++ran;
        return {};
    }).has_value());
    EXPECT_EQ(ran, 1);
    ASSERT_EQ(m_lock.m_paths.size(), 1u);
    EXPECT_TRUE(m_lock.m_paths[0].starts_with("/agent/mcp-auth-refresh-"));
    EXPECT_EQ(m_lock.m_paths[0].size(), std::string("/agent/mcp-auth-refresh-").size() + 16);
}

TEST_F(McpOauthStoreTest, InvalidUrlsAndBrokenFilesAreReported) {
    EXPECT_FALSE(m_store.load("docs", "not a url").has_value());
    ASSERT_TRUE(m_files.createDirectories("/agent").has_value());
    ASSERT_TRUE(m_files.writeFile("/agent/mcp-auth.json", "{oops").has_value());
    const auto loaded = m_store.load("docs", "https://mcp.example.com/");
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code, "invalid_auth_file");
}
