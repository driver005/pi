#include <gtest/gtest.h>
#include <sys/stat.h>
#include <cstdlib>

import std;
import pi.ai.file_credential_store;
import pi.base.posix_file_lock;
import pi.base.posix_file_system;
import pi.testing.fake_environment;
import pi.testing.scripted_process_runner;

class FileCredentialStoreTest : public testing::Test {
protected:
    FileCredentialStoreTest()
        : m_dir(std::string(std::getenv("TEST_TMPDIR")) + "/creds_" +
                testing::UnitTest::GetInstance()->current_test_info()->name()),
          m_processes([](const ProcessRequest&) -> Result<ProcessResult> {
              ProcessResult result;
              result.exitCode = 0;
              result.output = "cmd-key\n";
              return result;
          }),
          m_resolver(m_environment, m_processes),
          m_store(m_dir + "/agent/auth.json", m_files, m_lock, m_resolver) {
        std::filesystem::remove_all(m_dir);
    }

    Credential apiKey(const std::string& key) {
        Credential credential;
        credential.key = key;
        return credential;
    }

    std::string m_dir;
    PosixFileSystem m_files;
    PosixFileLock m_lock;
    FakeEnvironment m_environment{{{"MY_KEY", "env-key"}}};
    ScriptedProcessRunner m_processes;
    ConfigValueResolver m_resolver;
    FileCredentialStore m_store;
};

TEST_F(FileCredentialStoreTest, MissingFileReadsAsEmpty) {
    const auto read = m_store.read("anthropic");
    ASSERT_TRUE(read.has_value());
    EXPECT_FALSE(read->has_value());
    EXPECT_TRUE(m_store.list()->empty());
}

TEST_F(FileCredentialStoreTest, ModifyCreatesPrivateFileAndPersists) {
    const auto written = m_store.modify("anthropic", [&](const std::optional<Credential>& current) {
        EXPECT_FALSE(current.has_value());
        return Result<std::optional<Credential>>(std::optional<Credential>(apiKey("sk-1")));
    });
    ASSERT_TRUE(written.has_value());
    struct stat info {};
    ASSERT_EQ(stat((m_dir + "/agent/auth.json").c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600U);
    ASSERT_EQ(stat((m_dir + "/agent").c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0700U);
    const auto read = m_store.read("anthropic");
    ASSERT_TRUE(read->has_value());
    EXPECT_EQ((*read)->key, "sk-1");
    EXPECT_FALSE(std::filesystem::exists(m_dir + "/agent/auth.json.lock"));
}

TEST_F(FileCredentialStoreTest, ReadResolvesEnvAndCommandKeys) {
    ASSERT_TRUE(m_store.modify("a", [&](const auto&) { return Result<std::optional<Credential>>(std::optional<Credential>(apiKey("$MY_KEY"))); }));
    ASSERT_TRUE(m_store.modify("b", [&](const auto&) { return Result<std::optional<Credential>>(std::optional<Credential>(apiKey("!pass show b"))); }));
    ASSERT_TRUE(m_store.modify("c", [&](const auto&) { return Result<std::optional<Credential>>(std::optional<Credential>(apiKey("$MISSING"))); }));
    EXPECT_EQ((*m_store.read("a"))->key, "env-key");
    EXPECT_EQ((*m_store.read("b"))->key, "cmd-key");
    EXPECT_FALSE((*m_store.read("c"))->key.has_value());
    // The file keeps the raw reference.
    const std::string text = m_files.readFile(m_dir + "/agent/auth.json").value();
    EXPECT_NE(text.find("\"$MY_KEY\""), std::string::npos);
}

TEST_F(FileCredentialStoreTest, ModifyReturningNulloptLeavesEntryUnchanged) {
    ASSERT_TRUE(m_store.modify("a", [&](const auto&) { return Result<std::optional<Credential>>(std::optional<Credential>(apiKey("k"))); }));
    const auto result = m_store.modify("a", [](const std::optional<Credential>& current) {
        EXPECT_EQ(current->key, "k");
        return Result<std::optional<Credential>>(std::optional<Credential>());
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)->key, "k");
}

TEST_F(FileCredentialStoreTest, ModifyErrorPropagatesWithoutWriting) {
    const auto result = m_store.modify("a", [](const auto&) -> Result<std::optional<Credential>> {
        return std::unexpected(Error{"oauth", "refresh failed"});
    });
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "refresh failed");
    EXPECT_TRUE(m_store.list()->empty());
}

TEST_F(FileCredentialStoreTest, ListAndRemove) {
    ASSERT_TRUE(m_store.modify("a", [&](const auto&) { return Result<std::optional<Credential>>(std::optional<Credential>(apiKey("k"))); }));
    Credential oauth;
    oauth.type = CredentialType::OAuth;
    oauth.access = "x";
    oauth.refresh = "y";
    oauth.expires = 99;
    ASSERT_TRUE(m_store.modify("b", [&](const auto&) { return Result<std::optional<Credential>>(std::optional<Credential>(oauth)); }));
    const auto infos = m_store.list();
    ASSERT_EQ(infos->size(), 2U);
    EXPECT_EQ((*infos)[1].type, CredentialType::OAuth);
    ASSERT_TRUE(m_store.remove("a").has_value());
    EXPECT_EQ(m_store.list()->size(), 1U);
    EXPECT_FALSE((*m_store.read("a")).has_value());
}

TEST_F(FileCredentialStoreTest, ReadsFilesWrittenByTypeScript) {
    std::filesystem::create_directories(m_dir + "/agent");
    ASSERT_TRUE(m_files.writeFile(m_dir + "/agent/auth.json",
        "{\n  \"anthropic\": {\n    \"type\": \"oauth\",\n    \"refresh\": \"r\",\n    \"access\": \"a\",\n    \"expires\": 1700000000000\n  }\n}"));
    const auto read = m_store.read("anthropic");
    ASSERT_TRUE((*read).has_value());
    EXPECT_EQ((*read)->access, "a");
}

TEST_F(FileCredentialStoreTest, CorruptFileIsAnError) {
    std::filesystem::create_directories(m_dir + "/agent");
    ASSERT_TRUE(m_files.writeFile(m_dir + "/agent/auth.json", "{nope"));
    EXPECT_FALSE(m_store.read("x").has_value());
}
