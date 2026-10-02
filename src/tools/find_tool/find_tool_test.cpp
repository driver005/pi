#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.base.posix_file_system;
import pi.tools.find_tool;

class FindToolTest : public testing::Test {
protected:
    void write(const std::string& name, const std::string& content = "x") {
        ASSERT_TRUE(m_fs.createDirectories(std::filesystem::path(m_dir + "/" + name).parent_path().string()).has_value());
        ASSERT_TRUE(m_fs.writeFile(m_dir + "/" + name, content).has_value());
    }

    std::string find(const Json& params) {
        const auto result = m_tool.execute("c1", params, nullptr, nullptr);
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
        return result ? std::get<TextContent>(result->content[0]).text : "";
    }

    PosixFileSystem m_fs;
    std::string m_dir = [] {
        const std::string dir = std::string(std::getenv("TEST_TMPDIR")) + "/find_" +
                                testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }();
    FindTool m_tool{m_fs, m_dir};
};

TEST_F(FindToolTest, BasenamePatternMatchesAtAnyDepthSorted) {
    write("b.ts");
    write("a.ts");
    write("sub/c.ts");
    write("sub/d.js");
    EXPECT_EQ(find({{"pattern", "*.ts"}}), "a.ts\nb.ts\nsub/c.ts");
}

TEST_F(FindToolTest, PathPatternsMatchRelativePaths) {
    write("src/a/x.spec.ts");
    write("src/y.spec.ts");
    write("lib/z.spec.ts");
    EXPECT_EQ(find({{"pattern", "src/**/*.spec.ts"}}), "src/a/x.spec.ts\nsrc/y.spec.ts");
    EXPECT_EQ(find({{"pattern", "**/*.spec.ts"}}), "lib/z.spec.ts\nsrc/a/x.spec.ts\nsrc/y.spec.ts");
}

TEST_F(FindToolTest, RespectsGitignoreAndSkipsDotGit) {
    ASSERT_TRUE(m_fs.createDirectories(m_dir + "/.git").has_value());
    write(".gitignore", "build/\n*.log\n");
    write("build/out.ts");
    write("keep.ts");
    write("debug.log");
    write(".git/config");
    write(".hidden.ts");
    EXPECT_EQ(find({{"pattern", "*.ts"}}), ".hidden.ts\nkeep.ts");
    EXPECT_EQ(find({{"pattern", "*.log"}}), "No files found matching pattern");
}

TEST_F(FindToolTest, NestedGitignoreAppliesOnlyBelowItsDirectory) {
    write("pkg/.gitignore", "gen.ts\n");
    write("pkg/gen.ts");
    write("pkg/real.ts");
    write("gen.ts");
    EXPECT_EQ(find({{"pattern", "*.ts"}}), "gen.ts\npkg/real.ts");
}

TEST_F(FindToolTest, DirectoriesCanMatchAndGetTrailingSlash) {
    write("node/index.js");
    EXPECT_EQ(find({{"pattern", "node"}}), "node/");
}

TEST_F(FindToolTest, LimitNoticeAndMissingPath) {
    for (int i = 0; i < 5; ++i) {
        write("f" + std::to_string(i) + ".txt");
    }
    EXPECT_EQ(find({{"pattern", "*.txt"}, {"limit", 2}}),
              "f0.txt\nf1.txt\n\n[2 results limit reached. Use limit=4 for more, or refine pattern]");
    const auto missing = m_tool.execute("c", {{"pattern", "*"}, {"path", "nope"}}, nullptr, nullptr);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message, "Path not found: " + m_dir + "/nope");
}

TEST_F(FindToolTest, SearchInSubdirectoryReturnsPathsRelativeToIt) {
    write("sub/inner/a.md");
    EXPECT_EQ(find({{"pattern", "*.md"}, {"path", "sub"}}), "inner/a.md");
}
