#include <gtest/gtest.h>

import std;
import pi.support.package_resource_collector;
import pi.testing.fake_file_system;

class PackageResourceCollectorTest : public testing::Test {
protected:
    void put(const std::string& path, const std::string& content = "x") {
        m_files.createDirectories(path.substr(0, path.rfind('/')));
        m_files.writeFile(path, content);
    }

    std::vector<std::string> paths(const std::vector<PackageResourceEntry>& entries, bool enabled = true) {
        std::vector<std::string> out;
        for (const PackageResourceEntry& entry : entries) {
            if (entry.enabled == enabled) {
                out.push_back(entry.path);
            }
        }
        return out;
    }

    void conventionalPackage() {
        put("/pkg/skills/a/SKILL.md");
        put("/pkg/skills/b/SKILL.md");
        put("/pkg/skills/top.md");
        put("/pkg/skills/b/notes.md");
        put("/pkg/skills/.hidden/SKILL.md");
        put("/pkg/prompts/review.md");
        put("/pkg/prompts/readme.txt");
        put("/pkg/plugins/hello.so");
        put("/pkg/plugins/data.json");
    }

    FakeFileSystem m_files;
    PackageResourceCollector m_collector{m_files};
};

TEST_F(PackageResourceCollectorTest, ConventionalDirectories) {
    conventionalPackage();
    const PackageResources resources = m_collector.collect("/pkg", std::nullopt);
    EXPECT_EQ(paths(resources.skills), (std::vector<std::string>{"/pkg/skills/a/SKILL.md", "/pkg/skills/b/SKILL.md", "/pkg/skills/top.md"}));
    EXPECT_EQ(paths(resources.prompts), (std::vector<std::string>{"/pkg/prompts/review.md"}));
    EXPECT_EQ(paths(resources.plugins), (std::vector<std::string>{"/pkg/plugins/hello.so"}));
}

TEST_F(PackageResourceCollectorTest, ManifestNamesFilesDirectoriesAndGlobs) {
    conventionalPackage();
    put("/pkg/extra/p1.md");
    put("/pkg/extra/p2.md");
    put("/pkg/package.json", R"({"pi":{"prompts":["extra/*.md","!extra/p2.md"],"skills":["skills/a"]}})");
    const PackageResources resources = m_collector.collect("/pkg", std::nullopt);
    EXPECT_EQ(paths(resources.prompts), (std::vector<std::string>{"/pkg/extra/p1.md"}));
    EXPECT_EQ(paths(resources.skills), (std::vector<std::string>{"/pkg/skills/a/SKILL.md"}));
    EXPECT_TRUE(resources.plugins.empty()) << "a manifest without the key offers none";
}

TEST_F(PackageResourceCollectorTest, FiltersNarrowWhatThePackageOffers) {
    conventionalPackage();
    PackageFilter filter;
    filter.skills = std::vector<std::string>{"skills/a"};
    filter.prompts = std::vector<std::string>{};
    const PackageResources resources = m_collector.collect("/pkg", filter);
    EXPECT_EQ(paths(resources.skills), (std::vector<std::string>{"/pkg/skills/a/SKILL.md"}));
    EXPECT_EQ(paths(resources.skills, false).size(), 2u);
    EXPECT_TRUE(paths(resources.prompts).empty());
    EXPECT_EQ(paths(resources.prompts, false), (std::vector<std::string>{"/pkg/prompts/review.md"}));
    EXPECT_EQ(paths(resources.plugins), (std::vector<std::string>{"/pkg/plugins/hello.so"})) << "an absent list keeps the default";
}

TEST_F(PackageResourceCollectorTest, AutoloadFalseOnlyReportsTheNamedPaths) {
    conventionalPackage();
    PackageFilter filter;
    filter.autoload = false;
    filter.skills = std::vector<std::string>{"-skills/b", "+skills/top.md"};
    const PackageResources resources = m_collector.collect("/pkg", filter);
    ASSERT_EQ(resources.skills.size(), 2u);
    EXPECT_EQ(resources.skills[0].path, "/pkg/skills/b/SKILL.md");
    EXPECT_FALSE(resources.skills[0].enabled);
    EXPECT_EQ(resources.skills[1].path, "/pkg/skills/top.md");
    EXPECT_TRUE(resources.skills[1].enabled);
    EXPECT_TRUE(resources.prompts.empty());
}
