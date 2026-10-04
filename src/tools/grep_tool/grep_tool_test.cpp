#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.base.posix_file_system;
import pi.base.posix_process_runner;
import pi.tools.grep_tool;

class GrepToolTest : public testing::Test {
protected:
    void write(const std::string& name, const std::string& content) {
        ASSERT_TRUE(m_fs.createDirectories(std::filesystem::path(m_dir + "/" + name).parent_path().string()).has_value());
        ASSERT_TRUE(m_fs.writeFile(m_dir + "/" + name, content).has_value());
    }

    std::string grep(const Json& params) {
        const auto result = m_tool.execute("c1", params, nullptr, nullptr);
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
        return result ? std::get<TextContent>(result->content[0]).text : "";
    }

    PosixProcessRunner m_runner;
    PosixFileSystem m_fs;
    std::string m_dir = [] {
        const std::string dir = std::string(std::getenv("TEST_TMPDIR")) + "/grep_" +
                                testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }();
    GrepTool m_tool{m_runner, m_fs, m_dir};
};

TEST_F(GrepToolTest, FindsMatchesWithRelativePathsAndLineNumbers) {
    write("a.txt", "alpha\nbeta\nalpha beta\n");
    write("sub/b.txt", "nothing\nalpha here\n");
    std::vector<std::string> lines;
    std::istringstream stream(grep({{"pattern", "alpha"}}));
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(line);
    }
    std::sort(lines.begin(), lines.end());
    EXPECT_EQ(lines, (std::vector<std::string>{"a.txt:1: alpha", "a.txt:3: alpha beta", "sub/b.txt:2: alpha here"}));
}

TEST_F(GrepToolTest, NoMatches) {
    write("a.txt", "x\n");
    EXPECT_EQ(grep({{"pattern", "zzz"}}), "No matches found");
}

TEST_F(GrepToolTest, IgnoreCaseLiteralAndGlob) {
    write("a.txt", "Hello (world)\n");
    write("b.md", "hello (world)\n");
    EXPECT_EQ(grep({{"pattern", "(world)"}, {"literal", true}, {"glob", "*.txt"}}), "a.txt:1: Hello (world)");
    EXPECT_EQ(grep({{"pattern", "HELLO"}, {"ignoreCase", true}, {"glob", "*.md"}}), "b.md:1: hello (world)");
}

TEST_F(GrepToolTest, ContextLinesUseDashSeparators) {
    write("a.txt", "one\ntwo\nthree\nfour\nfive\n");
    EXPECT_EQ(grep({{"pattern", "three"}, {"context", 1}}), "a.txt-2- two\na.txt:3: three\na.txt-4- four");
}

TEST_F(GrepToolTest, LimitStopsEarlyWithNotice) {
    std::string content;
    for (int i = 0; i < 50; ++i) {
        content += "hit " + std::to_string(i) + "\n";
    }
    write("a.txt", content);
    const std::string output = grep({{"pattern", "hit"}, {"limit", 3}});
    EXPECT_NE(output.find("[3 matches limit reached. Use limit=6 for more, or refine pattern]"), std::string::npos);
    EXPECT_EQ(std::count(output.begin(), output.end(), ':') >= 3, true);
}

TEST_F(GrepToolTest, LongLinesAreTruncatedWithNotice) {
    write("a.txt", "needle " + std::string(800, 'x') + "\n");
    const std::string output = grep({{"pattern", "needle"}});
    EXPECT_NE(output.find("... [truncated]"), std::string::npos);
    EXPECT_NE(output.find("Some lines truncated to 500 chars"), std::string::npos);
}

TEST_F(GrepToolTest, SingleFileUsesBasenameAndErrorsAreReported) {
    write("dir/a.txt", "needle\n");
    EXPECT_EQ(grep({{"pattern", "needle"}, {"path", "dir/a.txt"}}), "a.txt:1: needle");
    const auto missing = m_tool.execute("c", {{"pattern", "x"}, {"path", "nope"}}, nullptr, nullptr);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message, "Path not found: " + m_dir + "/nope");
    const auto badRegex = m_tool.execute("c", {{"pattern", "("}}, nullptr, nullptr);
    ASSERT_FALSE(badRegex.has_value());
    EXPECT_NE(badRegex.error().message.find("regex"), std::string::npos);
}
