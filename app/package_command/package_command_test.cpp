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

    /**
     * Makes `npm` the program at `command` (npmCommand setting); with an empty command a script that behaves like npm for the
     * calls pi makes: install and uninstall under --prefix, and `view` answering the version in <agent>/latest.
     */
    std::string writeFakeNpm(const std::string& command) {
        std::string path = command;
        if (path.empty()) {
            path = m_dir + "/fake-npm.sh";
            std::ofstream(path) << R"sh(#!/bin/sh
dir=$(dirname "$0")
verb=$1
shift
case "$verb" in
  view) printf '"%s"
' "$(cat "$dir/latest")";;
  install)
    spec=$1; root=
    while [ $# -gt 0 ]; do [ "$1" = "--prefix" ] && root=$2; shift; done
    case "$spec" in
      @*) rest=${spec#@}; name=@${rest%%@*}; version=${rest#*@}; [ "$rest" = "${rest#*@}" ] && version=latest;;
      *) name=${spec%%@*}; version=${spec#*@}; [ "$spec" = "$version" ] && version=latest;;
    esac
    [ "$version" = latest ] && version=$(cat "$dir/latest")
    mkdir -p "$root/node_modules/$name/skills/greet"
    printf -- '---
name: greet
description: Greets
---
Hello
' > "$root/node_modules/$name/skills/greet/SKILL.md"
    printf '{"name":"%s","version":"%s"}' "$name" "$version" > "$root/node_modules/$name/package.json";;
  uninstall) name=$1; root=; while [ $# -gt 0 ]; do [ "$1" = "--prefix" ] && root=$2; shift; done; rm -rf "$root/node_modules/$name";;
esac
)sh";
            std::filesystem::permissions(path, std::filesystem::perms::owner_all);
        }
        std::ofstream(m_dir + "/agent/settings.json") << nlohmann::json{{"npmCommand", nlohmann::json::array({path})}}.dump();
        return path;
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

TEST_F(PackageCommandTest, InstallUpdateAndRemoveAnNpmPackage) {
    writeFakeNpm("");
    std::ofstream(m_dir + "/latest") << "1.0.0";
    ASSERT_EQ(run("install", {"npm:@acme/tool"}), 0) << m_err.str();
    EXPECT_EQ(m_out.str(), "Installed npm:@acme/tool\n");
    const std::string package = m_dir + "/agent/npm/node_modules/@acme/tool";
    EXPECT_NE(read(package + "/package.json").find("1.0.0"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(m_dir + "/agent/npm/package.json"));
    const auto settings = nlohmann::json::parse(read(m_dir + "/agent/settings.json"));
    EXPECT_EQ(settings["packages"], nlohmann::json::parse(R"(["npm:@acme/tool"])"));
    EXPECT_TRUE(settings.contains("npmCommand"));

    ASSERT_EQ(run("list", {}), 0);
    EXPECT_NE(m_out.str().find(package), std::string::npos);

    std::ofstream(m_dir + "/latest") << "1.1.0";
    ASSERT_EQ(run("update", {}), 0) << m_err.str();
    EXPECT_EQ(m_out.str(), "Updated npm:@acme/tool\n");
    EXPECT_NE(read(package + "/package.json").find("1.1.0"), std::string::npos);

    ASSERT_EQ(run("remove", {"npm:@acme/tool"}), 0) << m_err.str();
    EXPECT_FALSE(std::filesystem::exists(package));
    ASSERT_EQ(run("list", {}), 0);
    EXPECT_EQ(m_out.str(), "No packages installed.\n");
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
    writeFakeNpm("/bin/false");
    EXPECT_EQ(run("install", {"npm:left-pad"}), 1);
    EXPECT_NE(m_err.str().find("failed"), std::string::npos);
    EXPECT_EQ(run("update", {"git:example.test/nobody/nothing"}), 1);
}
