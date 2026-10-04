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

    /** A scripted npm: installs write node_modules/<name> with a version and a skill, `view` answers m_latest. */
    Result<ProcessResult> npm(const ProcessRequest& request) {
        ProcessResult ok;
        ok.exitCode = 0;
        const std::string& verb = request.args.at(0);
        const auto prefix = std::ranges::find(request.args, "--prefix");
        if (verb == "view") {
            ok.output = "\"" + m_latest + "\"\n";
            return ok;
        }
        const std::string root = *(prefix + 1);
        if (verb == "install") {
            if (m_npmFails) {
                ok.exitCode = 1;
                ok.output = "npm ERR! 404\n";
                return ok;
            }
            const std::string spec = request.args.at(1);
            const std::size_t at = spec.find('@', spec.starts_with("@") ? 1 : 0);
            const std::string name = spec.substr(0, at);
            const std::string version = at == std::string::npos || spec.substr(at + 1) == "latest" ? m_latest : spec.substr(at + 1);
            m_files.createDirectories(root + "/node_modules/" + name + "/skills/n1");
            m_files.writeFile(root + "/node_modules/" + name + "/skills/n1/SKILL.md", "x");
            m_files.writeFile(root + "/node_modules/" + name + "/package.json", "{\"name\":\"" + name + "\",\"version\":\"" + version + "\"}");
        } else if (verb == "uninstall") {
            m_files.removeTree(root + "/node_modules/" + request.args.at(1));
        }
        return ok;
    }

    std::vector<std::vector<std::string>> npmCalls() {
        std::vector<std::vector<std::string>> out;
        for (const ProcessRequest& request : m_git.requests()) {
            if (request.command == "npm") {
                out.push_back(request.args);
            }
        }
        return out;
    }

    Result<ProcessResult> git(const ProcessRequest& request) {
        if (request.command == "npm") {
            return npm(request);
        }
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
    bool m_npmFails = false;
    std::string m_latest = "2.0.0";
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
    EXPECT_TRUE(resolution.warnings.empty());
}

TEST_F(PackageManagerTest, InstallsNpmPackagesIntoTheManagedProjectAndRecordsThem) {
    PackageManager packages = manager();
    ASSERT_TRUE(packages.install("npm:@acme/tool@1.2.3", false));
    EXPECT_EQ(npmCalls(), (std::vector<std::vector<std::string>>{{"install", "@acme/tool@1.2.3", "--prefix", "/agent/npm", "--legacy-peer-deps"}}));
    EXPECT_EQ(m_settings.globalSettings()["packages"], Json::parse(R"(["npm:@acme/tool@1.2.3"])"));
    const auto listed = packages.list();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0].installedPath, "/agent/npm/node_modules/@acme/tool");
    const PackagePaths paths = packages.load(false);
    EXPECT_EQ(paths.skills, (std::vector<std::string>{"/agent/npm/node_modules/@acme/tool/skills/n1/SKILL.md"}));
}

TEST_F(PackageManagerTest, ProjectNpmPackagesNeedATrustedProject) {
    PackageManager packages = manager();
    ASSERT_TRUE(packages.install("npm:tool", true));
    EXPECT_EQ(npmCalls()[0], (std::vector<std::string>{"install", "tool", "--prefix", "/work/.pi/npm", "--legacy-peer-deps"}));
    m_settings.setProjectTrusted(false);
    const auto untrusted = packages.install("npm:other", true);
    ASSERT_FALSE(untrusted);
    EXPECT_EQ(untrusted.error().code, "untrusted_project");
    EXPECT_EQ(npmCalls().size(), 1u);
}

TEST_F(PackageManagerTest, AFailedNpmInstallRecordsNothing) {
    m_npmFails = true;
    PackageManager packages = manager();
    const auto result = packages.install("npm:tool", false);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("npm ERR! 404"), std::string::npos);
    EXPECT_TRUE(m_settings.globalSettings()["packages"].is_null());
}

TEST_F(PackageManagerTest, RemovingAnNpmPackageUninstallsItAndDropsTheEntry) {
    PackageManager packages = manager();
    ASSERT_TRUE(packages.install("npm:tool@1.0.0", false));
    const auto removed = packages.remove("npm:tool@1.0.0", false);
    ASSERT_TRUE(removed);
    EXPECT_TRUE(*removed);
    EXPECT_EQ(npmCalls().back(), (std::vector<std::string>{"uninstall", "tool", "--prefix", "/agent/npm", "--legacy-peer-deps"}));
    EXPECT_FALSE(m_files.exists("/agent/npm/node_modules/tool"));
    EXPECT_TRUE(packages.list().empty());
}

TEST_F(PackageManagerTest, ResolvingInstallsAMissingNpmPackageAndReinstallsAWrongPinnedVersion) {
    m_settings.setGlobal("packages", Json::parse(R"(["npm:tool@1.0.0"])"));
    PackageManager packages = manager();
    EXPECT_TRUE(packages.load(false).skills.empty());
    EXPECT_TRUE(npmCalls().empty());
    EXPECT_EQ(packages.load(true).skills.size(), 1u);
    EXPECT_EQ(npmCalls().size(), 1u);
    EXPECT_EQ(packages.load(true).skills.size(), 1u);
    EXPECT_EQ(npmCalls().size(), 1u) << "the pinned version is there";

    m_files.writeFile("/agent/npm/node_modules/tool/package.json", R"({"name":"tool","version":"0.9.0"})");
    EXPECT_EQ(packages.load(true).skills.size(), 1u);
    EXPECT_EQ(npmCalls().size(), 2u) << "a wrong version of a pinned package is installed again";
}

TEST_F(PackageManagerTest, UpdatingNpmPackagesInstallsOnlyNewerVersionsAndLeavesPinnedOnesAlone) {
    m_settings.setGlobal("packages", Json::parse(R"(["npm:latest-tool", "npm:ranged@^1.0.0", "npm:pinned@1.0.0"])"));
    PackageManager packages = manager();
    for (const char* name : {"latest-tool", "ranged", "pinned"}) {
        m_files.createDirectories(std::string("/agent/npm/node_modules/") + name);
        m_files.writeFile(std::string("/agent/npm/node_modules/") + name + "/package.json", std::string("{\"version\":\"1.0.0\"}"));
    }
    m_latest = "1.0.0";
    auto updated = packages.update(std::nullopt);
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated->size(), 2u);
    for (const auto& call : npmCalls()) {
        EXPECT_EQ(call.at(0), "view") << "nothing newer: no install";
    }

    m_latest = "1.5.0";
    ASSERT_TRUE(packages.update(std::nullopt));
    std::vector<std::string> installs;
    for (const auto& call : npmCalls()) {
        if (call.at(0) == "install") {
            installs.push_back(call.at(1));
        }
    }
    EXPECT_EQ(installs, (std::vector<std::string>{"latest-tool@latest", "ranged@^1.0.0"}));
    EXPECT_EQ(npmCalls()[0].at(1), "latest-tool") << "the registry is asked about the name, or the specification with a version";
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
