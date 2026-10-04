#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.package_command;

/** Runs the commands against a real git repository: `https://example.test/` is redirected to a local directory. */
class PackageCommandTest : public testing::Test {
protected:
    PackageCommandTest() {
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/package_command_" + testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir + "/agent");
        std::filesystem::create_directories(m_dir + "/work");
        std::filesystem::create_directories(m_dir + "/remote/user");
        setenv("GIT_CONFIG_COUNT", "1", 1);
        setenv("GIT_CONFIG_KEY_0", ("url.file://" + m_dir + "/remote/.insteadOf").c_str(), 1);
        setenv("GIT_CONFIG_VALUE_0", "https://example.test/", 1);
        setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1);
        setenv("GIT_CONFIG_SYSTEM", "/dev/null", 1);
        m_services = std::make_unique<CodingServices>(m_dir + "/agent", m_dir + "/agent/catalog", true);
    }

    ~PackageCommandTest() override {
        unsetenv("GIT_CONFIG_COUNT");
        unsetenv("GIT_CONFIG_KEY_0");
        unsetenv("GIT_CONFIG_VALUE_0");
    }

    void shell(const std::string& command) {
        ASSERT_EQ(std::system(command.c_str()), 0) << command;
    }

    /** A repository with one skill; each call commits another version of its prompt. */
    void publish(const std::string& version) {
        const std::string repo = m_dir + "/remote/user/tool";
        if (!std::filesystem::exists(repo)) {
            std::filesystem::create_directories(repo + "/skills/greet");
            std::ofstream(repo + "/skills/greet/SKILL.md") << "---\nname: greet\ndescription: Greets\n---\nHello\n";
            shell("git -C " + repo + " init -q -b main");
        }
        std::filesystem::create_directories(repo + "/prompts");
        std::ofstream(repo + "/prompts/ask.md") << version << "\n";
        shell("git -C " + repo + " add -A && git -C " + repo + " -c user.name=t -c user.email=t@t commit -q -m " + version);
    }

    int run(const std::string& command, const std::vector<std::string>& arguments, bool local = false) {
        CommandLine line;
        line.command = command;
        line.arguments = arguments;
        line.localPackages = local;
        line.options.agentDir = m_dir + "/agent";
        line.options.cwd = m_dir + "/work";
        line.options.startup.trustProject = true;
        m_out.str("");
        m_err.str("");
        PackageCommand packages(*m_services, m_out, m_err);
        return packages.run(line);
    }

    std::string read(const std::string& path) {
        std::ifstream in(path);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::string m_dir;
    std::unique_ptr<CodingServices> m_services;
    std::ostringstream m_out;
    std::ostringstream m_err;
};

TEST_F(PackageCommandTest, InstallListUpdateAndRemoveAGitPackage) {
    publish("one");
    ASSERT_EQ(run("install", {"git:example.test/user/tool"}), 0) << m_err.str();
    EXPECT_EQ(m_out.str(), "Installed git:example.test/user/tool\n");
    const std::string checkout = m_dir + "/agent/git/example.test/user/tool";
    EXPECT_EQ(read(checkout + "/prompts/ask.md"), "one\n");
    const auto settings = nlohmann::json::parse(read(m_dir + "/agent/settings.json"));
    EXPECT_EQ(settings["packages"], nlohmann::json::parse(R"(["git:example.test/user/tool"])"));

    ASSERT_EQ(run("list", {}), 0);
    EXPECT_NE(m_out.str().find("User packages:"), std::string::npos);
    EXPECT_NE(m_out.str().find(checkout), std::string::npos);

    publish("two");
    ASSERT_EQ(run("update", {}), 0) << m_err.str();
    EXPECT_EQ(m_out.str(), "Updated git:example.test/user/tool\n");
    EXPECT_EQ(read(checkout + "/prompts/ask.md"), "two\n");

    ASSERT_EQ(run("remove", {"git:example.test/user/tool"}), 0) << m_err.str();
    EXPECT_FALSE(std::filesystem::exists(m_dir + "/agent/git/example.test"));
    ASSERT_EQ(run("list", {}), 0);
    EXPECT_EQ(m_out.str(), "No packages installed.\n");
    EXPECT_EQ(run("remove", {"git:example.test/user/tool"}), 1);
    EXPECT_NE(m_err.str().find("No matching package"), std::string::npos);
}

TEST_F(PackageCommandTest, PinnedRefsInstallThatRefAndStayThere) {
    publish("one");
    shell("git -C " + m_dir + "/remote/user/tool tag v1");
    publish("two");
    ASSERT_EQ(run("install", {"git:example.test/user/tool@v1"}), 0) << m_err.str();
    const std::string checkout = m_dir + "/agent/git/example.test/user/tool";
    EXPECT_EQ(read(checkout + "/prompts/ask.md"), "one\n");
    ASSERT_EQ(run("update", {}), 0);
    EXPECT_EQ(read(checkout + "/prompts/ask.md"), "one\n");
}

TEST_F(PackageCommandTest, LocalPackagesGoToTheProjectSettings) {
    std::filesystem::create_directories(m_dir + "/work/.pi/mine/prompts");
    ASSERT_EQ(run("install", {"./.pi/mine"}, true), 0) << m_err.str();
    const auto settings = nlohmann::json::parse(read(m_dir + "/work/.pi/settings.json"));
    EXPECT_EQ(settings["packages"], nlohmann::json::parse(R"(["mine"])"));
    EXPECT_FALSE(std::filesystem::exists(m_dir + "/agent/settings.json"));
}

TEST_F(PackageCommandTest, ReportsFailures) {
    EXPECT_EQ(run("install", {"git:example.test/user/missing"}), 1);
    EXPECT_FALSE(m_err.str().empty());
    EXPECT_FALSE(std::filesystem::exists(m_dir + "/agent/git/example.test"));
    EXPECT_EQ(run("install", {"npm:left-pad"}), 1);
    EXPECT_NE(m_err.str().find("npm packages are not supported"), std::string::npos);
    EXPECT_EQ(run("update", {"git:example.test/nobody/nothing"}), 1);
}
