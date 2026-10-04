#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.npm_command;
import pi.testing.fake_file_system;
import pi.testing.fake_settings_manager;
import pi.testing.scripted_process_runner;

class NpmCommandTest : public testing::Test {
protected:
    NpmCommandTest()
        : m_runner([this](const ProcessRequest& request) -> Result<ProcessResult> {
              ProcessResult result;
              result.exitCode = m_exitCode;
              result.output = m_output;
              (void)request;
              return result;
          }) {}

    NpmCommand make() {
        return NpmCommand(m_files, m_runner, m_settings);
    }

    FakeFileSystem m_files;
    FakeSettingsManager m_settings;
    int m_exitCode = 0;
    std::string m_output;
    ScriptedProcessRunner m_runner;
};

TEST_F(NpmCommandTest, InstallsIntoAManagedProject) {
    NpmCommand npm = make();
    ASSERT_TRUE(npm.install({"@scope/pkg@1.2.3", "other"}, "/agent/npm").has_value());
    ASSERT_EQ(m_runner.requests().size(), 1U);
    const ProcessRequest request = m_runner.requests()[0];
    EXPECT_EQ(request.command, "npm");
    EXPECT_EQ(request.args, (std::vector<std::string>{"install", "@scope/pkg@1.2.3", "other", "--prefix", "/agent/npm", "--legacy-peer-deps"}));
    EXPECT_EQ(Json::parse(*m_files.readFile("/agent/npm/package.json"))["name"], "pi-extensions");
    EXPECT_EQ(*m_files.readFile("/agent/npm/.gitignore"), "*\n!.gitignore\n");

    ASSERT_TRUE(m_files.writeFile("/agent/npm/package.json", R"({"name":"mine"})").has_value());
    ASSERT_TRUE(npm.install({"x"}, "/agent/npm").has_value());
    EXPECT_EQ(Json::parse(*m_files.readFile("/agent/npm/package.json"))["name"], "mine") << "an existing project is kept";
}

TEST_F(NpmCommandTest, UsesTheConfiguredCommandAndItsPackageManagersArguments) {
    m_settings.setGlobal("npmCommand", Json::array({"mise", "exec", "node@22", "--", "pnpm"}));
    NpmCommand npm = make();
    EXPECT_EQ(npm.packageManagerName(), "pnpm");
    ASSERT_TRUE(npm.install({"pkg"}, "/r").has_value());
    const ProcessRequest request = m_runner.requests()[0];
    EXPECT_EQ(request.command, "mise");
    EXPECT_EQ(request.args, (std::vector<std::string>{"exec", "node@22", "--", "pnpm", "install", "pkg", "--prefix", "/r", "--config.auto-install-peers=false", "--config.strict-peer-dependencies=false", "--config.strict-dep-builds=false"}));

    m_settings.setGlobal("npmCommand", Json::array({"/usr/bin/bun.exe"}));
    EXPECT_EQ(npm.packageManagerName(), "bun");
    ASSERT_TRUE(npm.install({"pkg"}, "/r").has_value());
    EXPECT_EQ(m_runner.requests()[1].args, (std::vector<std::string>{"install", "pkg", "--cwd", "/r", "--omit=peer"}));

    m_settings.setGlobal("npmCommand", Json::array({"wrapper", "npm"}));
    EXPECT_EQ(npm.packageManagerName(), "npm");
    m_settings.setGlobal("npmCommand", Json::array({"wrapper", "npm", "pnpm"}));
    EXPECT_EQ(npm.packageManagerName(), "wrapper") << "ambiguous wrappers fall back to the command itself";
}

TEST_F(NpmCommandTest, UninstallsOnlyWhenTheProjectExists) {
    NpmCommand npm = make();
    ASSERT_TRUE(npm.uninstall("pkg", "/agent/npm").has_value());
    EXPECT_EQ(m_runner.calls(), 0);
    ASSERT_TRUE(m_files.createDirectories("/agent/npm").has_value());
    ASSERT_TRUE(npm.uninstall("pkg", "/agent/npm").has_value());
    EXPECT_EQ(m_runner.requests()[0].args, (std::vector<std::string>{"uninstall", "pkg", "--prefix", "/agent/npm", "--legacy-peer-deps"}));
}

TEST_F(NpmCommandTest, FailuresCarryTheCommandsOutput) {
    NpmCommand npm = make();
    m_exitCode = 1;
    m_output = "\nnpm ERR! 404 Not Found\n";
    const auto failed = npm.install({"nope"}, "/r");
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "npm install failed: npm ERR! 404 Not Found");
}

TEST_F(NpmCommandTest, ReadsTheNewestVersionFromTheRegistryAnswer) {
    NpmCommand npm = make();
    m_output = "\"1.4.0\"\n";
    EXPECT_EQ(npm.latestVersion("pkg", "/work"), "1.4.0");
    EXPECT_EQ(m_runner.requests()[0].args, (std::vector<std::string>{"view", "pkg", "version", "--json"}));
    EXPECT_EQ(m_runner.requests()[0].cwd, "/work");
    m_output = R"(["1.2.0","1.10.0","1.9.9", 3])";
    EXPECT_EQ(npm.latestVersion("pkg@^1", "/work"), "1.10.0");
    m_output = "{}";
    EXPECT_FALSE(npm.latestVersion("pkg", "/work").has_value());
}

TEST_F(NpmCommandTest, ReadsTheInstalledVersion) {
    NpmCommand npm = make();
    EXPECT_FALSE(npm.installedVersion("/agent/npm/node_modules/pkg").has_value());
    ASSERT_TRUE(m_files.createDirectories("/agent/npm/node_modules/pkg").has_value());
    ASSERT_TRUE(m_files.createDirectories("/agent/npm/node_modules/bad").has_value());
    ASSERT_TRUE(m_files.writeFile("/agent/npm/node_modules/pkg/package.json", "\xEF\xBB\xBF{\"name\":\"pkg\",\"version\":\"2.0.1\"}").has_value());
    EXPECT_EQ(npm.installedVersion("/agent/npm/node_modules/pkg"), "2.0.1");
    ASSERT_TRUE(m_files.writeFile("/agent/npm/node_modules/bad/package.json", "not json").has_value());
    EXPECT_FALSE(npm.installedVersion("/agent/npm/node_modules/bad").has_value());
}
