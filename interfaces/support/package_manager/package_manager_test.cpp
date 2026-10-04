#include <gtest/gtest.h>

import std;
import pi.support.package_manager;
import pi.testing.fake_file_system;
import pi.testing.fake_settings_manager;
import pi.testing.scripted_process_runner;

class PackageManagerTest : public testing::Test {
protected:
    void SetUp() override {
        m_files.createDirectories("/work/.pi");
        m_files.createDirectories("/agent");
    }

    Result<ProcessResult> git(const ProcessRequest& request) {
        ProcessResult ok;
        ok.exitCode = 0;
        if (request.args.empty()) {
            return ok;
        }
        const std::string& verb = request.args[0];
        if (verb == "clone") {
            if (m_cloneFails) {
                ok.exitCode = 128;
                ok.output = "fatal: repository not found\n";
                return ok;
            }
            m_files.createDirectories(request.args[2] + "/skills/s1");
            m_files.writeFile(request.args[2] + "/skills/s1/SKILL.md", "x");
        } else if (verb == "rev-parse" && request.args[1] == "--abbrev-ref") {
            ok.output = "origin/main\n";
        } else if (verb == "rev-parse") {
            ok.output = request.args[1] == "HEAD" ? m_head : m_target;
        }
        return ok;
    }

    std::vector<std::string> verbs() {
        std::vector<std::string> out;
        for (const ProcessRequest& request : m_git.requests()) {
            out.push_back(request.args.empty() ? "" : request.args[0]);
        }
        return out;
    }

    PackageManager manager() {
        return PackageManager(m_files, m_git, m_settings, "/work", "/agent");
    }

    FakeFileSystem m_files{"/home/me"};
    FakeSettingsManager m_settings;
    ScriptedProcessRunner m_git{[this](const ProcessRequest& request) { return git(request); }};
    bool m_cloneFails = false;
    std::string m_head = "aaa\n";
    std::string m_target = "aaa\n";
};

TEST_F(PackageManagerTest, InstallsGitPackagesAndRecordsThem) {
    PackageManager packages = manager();
    ASSERT_TRUE(packages.install("git:github.com/user/repo@v1", false));
    EXPECT_EQ(verbs(), (std::vector<std::string>{"clone", "checkout"}));
    EXPECT_EQ(m_git.requests()[0].args, (std::vector<std::string>{"clone", "https://github.com/user/repo", "/agent/git/github.com/user/repo"}));
    EXPECT_EQ(m_git.requests()[1].args, (std::vector<std::string>{"checkout", "v1"}));
    EXPECT_EQ(m_git.requests()[1].cwd, "/agent/git/github.com/user/repo");
    EXPECT_EQ(m_git.requests()[0].env.at("GIT_TERMINAL_PROMPT"), "0");
    EXPECT_EQ(*m_files.readFile("/agent/git/.gitignore"), "*\n!.gitignore\n");
    EXPECT_EQ(m_settings.globalSettings()["packages"], Json::parse(R"(["git:github.com/user/repo@v1"])"));
    const auto listed = packages.list();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0].scope, "user");
    EXPECT_EQ(listed[0].installedPath, "/agent/git/github.com/user/repo");
}

TEST_F(PackageManagerTest, FailedCloneLeavesNothingBehind) {
    m_cloneFails = true;
    PackageManager packages = manager();
    const auto result = packages.install("git:github.com/user/repo", false);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("repository not found"), std::string::npos);
    EXPECT_FALSE(m_files.exists("/agent/git/github.com"));
    EXPECT_TRUE(m_settings.globalSettings()["packages"].is_null());
}

TEST_F(PackageManagerTest, RefusesWhatItCannotInstall) {
    PackageManager packages = manager();
    EXPECT_FALSE(packages.install("npm:left-pad", false));
    EXPECT_FALSE(packages.install("./missing", false));
    m_settings.setProjectTrusted(false);
    const auto untrusted = packages.install("git:github.com/user/repo", true);
    ASSERT_FALSE(untrusted);
    EXPECT_EQ(untrusted.error().code, "untrusted_project");
    EXPECT_EQ(m_git.calls(), 0);
}

TEST_F(PackageManagerTest, LocalPackagesAreRecordedRelativeToTheirScope) {
    m_files.createDirectories("/work/.pi/mine");
    PackageManager packages = manager();
    ASSERT_TRUE(packages.install("./.pi/mine", true));
    EXPECT_EQ(m_settings.projectSettings()["packages"], Json::parse(R"(["mine"])"));
    EXPECT_EQ(m_git.calls(), 0);
    const auto listed = packages.list();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0].installedPath, "/work/.pi/mine");
    const auto removed = packages.remove("./.pi/mine", true);
    ASSERT_TRUE(removed);
    EXPECT_TRUE(*removed);
    EXPECT_TRUE(m_files.exists("/work/.pi/mine")) << "local packages are never deleted";
    EXPECT_TRUE(m_settings.projectSettings()["packages"].empty());
}

TEST_F(PackageManagerTest, RemoveDeletesTheCheckoutAndEmptyParents) {
    PackageManager packages = manager();
    ASSERT_TRUE(packages.install("git:github.com/user/repo", false));
    const auto removed = packages.remove("git:github.com/user/repo@other", false);
    ASSERT_TRUE(removed);
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(m_files.exists("/agent/git/github.com"));
    EXPECT_TRUE(m_files.exists("/agent/git/.gitignore"));
    const auto again = packages.remove("git:github.com/user/repo", false);
    ASSERT_TRUE(again);
    EXPECT_FALSE(*again);
}

TEST_F(PackageManagerTest, UpdateMovesToTheUpstreamTipOnlyWhenItChanged) {
    PackageManager packages = manager();
    ASSERT_TRUE(packages.install("git:github.com/user/repo", false));
    const auto before = m_git.calls();
    const auto same = packages.update(std::nullopt);
    ASSERT_TRUE(same);
    EXPECT_EQ(*same, (std::vector<std::string>{"git:github.com/user/repo"}));
    for (const std::string& verb : verbs()) {
        EXPECT_NE(verb, "reset");
    }
    EXPECT_GT(m_git.calls(), before);
    m_target = "bbb\n";
    ASSERT_TRUE(packages.update("git:github.com/user/repo"));
    const auto all = m_git.requests();
    const auto reset = std::ranges::find_if(all, [](const ProcessRequest& request) { return request.args[0] == "reset"; });
    ASSERT_NE(reset, all.end());
    EXPECT_EQ(reset->args, (std::vector<std::string>{"reset", "--hard", "bbb"}));
    EXPECT_EQ(all.back().args[0], "clean");
}

TEST_F(PackageManagerTest, UpdateKeepsPinnedPackagesAndReportsUnknownOnes) {
    PackageManager packages = manager();
    ASSERT_TRUE(packages.install("git:github.com/user/repo@v1", false));
    const auto calls = m_git.calls();
    const auto pinned = packages.update(std::nullopt);
    ASSERT_TRUE(pinned);
    EXPECT_TRUE(pinned->empty());
    EXPECT_EQ(m_git.calls(), calls);
    EXPECT_FALSE(packages.update("git:github.com/nobody/nothing"));
}

TEST_F(PackageManagerTest, ResolveOffersThePackageResourcesAndProjectWins) {
    m_files.createDirectories("/work/.pi/proj/prompts");
    m_files.writeFile("/work/.pi/proj/prompts/p.md", "x");
    m_files.createDirectories("/agent/usr/skills/a");
    m_files.writeFile("/agent/usr/skills/a/SKILL.md", "x");
    m_files.createDirectories("/agent/usr/prompts");
    m_files.writeFile("/agent/usr/prompts/q.md", "x");
    m_settings.setGlobal("packages", Json::parse(R"(["usr", "git:github.com/user/repo"])"));
    m_settings.setProject("packages", Json::parse(R"(["proj", {"source":"../../agent/usr","prompts":[]}])"));
    PackageManager packages = manager();
    const PackageResolution resolution = packages.resolve(true);
    EXPECT_TRUE(resolution.warnings.empty());
    EXPECT_EQ(packages.enabled(resolution.resources.prompts), (std::vector<std::string>{"/work/.pi/proj/prompts/p.md"}));
    EXPECT_EQ(resolution.resources.prompts.size(), 2u);
    EXPECT_FALSE(resolution.resources.prompts[1].enabled) << "the project's `[]` turns the user package's prompts off";
    EXPECT_EQ(packages.enabled(resolution.resources.skills),
              (std::vector<std::string>{"/agent/usr/skills/a/SKILL.md", "/agent/git/github.com/user/repo/skills/s1/SKILL.md"}));
    EXPECT_EQ(verbs(), (std::vector<std::string>{"clone"})) << "the missing git package is cloned once";
}

TEST_F(PackageManagerTest, LoadListsTheEnabledFilesOfEachKind) {
    m_files.createDirectories("/agent/usr/plugins");
    m_files.createDirectories("/agent/usr/prompts");
    m_files.createDirectories("/agent/usr/skills/a");
    m_files.writeFile("/agent/usr/plugins/p.so", "x");
    m_files.writeFile("/agent/usr/prompts/q.md", "x");
    m_files.writeFile("/agent/usr/skills/a/SKILL.md", "x");
    m_settings.setGlobal("packages", Json::parse(R"([{"source":"usr","prompts":[]}])"));
    PackageManager packages = manager();
    const PackagePaths paths = packages.load(false);
    EXPECT_EQ(paths.plugins, (std::vector<std::string>{"/agent/usr/plugins/p.so"}));
    EXPECT_EQ(paths.skills, (std::vector<std::string>{"/agent/usr/skills/a/SKILL.md"}));
    EXPECT_TRUE(paths.prompts.empty());
    EXPECT_TRUE(paths.warnings.empty());
}

TEST_F(PackageManagerTest, ResolveWithoutInstallingSkipsMissingPackages) {
    m_settings.setGlobal("packages", Json::parse(R"(["git:github.com/user/repo", "npm:thing"])"));
    PackageManager packages = manager();
    const PackageResolution resolution = packages.resolve(false);
    EXPECT_TRUE(resolution.resources.skills.empty());
    EXPECT_EQ(m_git.calls(), 0);
    ASSERT_EQ(resolution.warnings.size(), 1u);
    EXPECT_NE(resolution.warnings[0].find("npm"), std::string::npos);
}

TEST_F(PackageManagerTest, AutoloadFalseAdjustsTheUserPackage) {
    m_files.createDirectories("/agent/usr/skills/a");
    m_files.createDirectories("/agent/usr/skills/b");
    m_files.writeFile("/agent/usr/skills/a/SKILL.md", "x");
    m_files.writeFile("/agent/usr/skills/b/SKILL.md", "x");
    m_settings.setGlobal("packages", Json::parse(R"(["usr"])"));
    m_settings.setProject("packages", Json::parse(R"([{"source":"../../agent/usr","autoload":false,"skills":["-skills/b"]}])"));
    PackageManager packages = manager();
    const PackageResolution resolution = packages.resolve(false);
    EXPECT_EQ(packages.enabled(resolution.resources.skills), (std::vector<std::string>{"/agent/usr/skills/a/SKILL.md"}));
    EXPECT_EQ(resolution.resources.skills.size(), 2u);
}
