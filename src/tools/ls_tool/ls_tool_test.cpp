#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.base.posix_file_system;
import pi.tools.ls_tool;

class LsToolTest : public testing::Test {
protected:
    std::string list(const Json& params) {
        const auto result = m_tool.execute("c1", params, nullptr, nullptr);
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
        return std::get<TextContent>(result->content[0]).text;
    }

    PosixFileSystem m_fs;
    std::string m_dir = [] {
        const std::string dir = std::string(std::getenv("TEST_TMPDIR")) + "/ls_" +
                                testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }();
    LsTool m_tool{m_fs, m_dir};
};

TEST_F(LsToolTest, SortsCaseInsensitivelyMarksDirectoriesAndIncludesDotfiles) {
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/b.txt", "").has_value());
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/A.txt", "").has_value());
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/.hidden", "").has_value());
    ASSERT_TRUE(m_fs.createDirectories(m_dir + "/sub").has_value());
    EXPECT_EQ(list(Json::object()), ".hidden\nA.txt\nb.txt\nsub/");
}

TEST_F(LsToolTest, EmptyDirectory) {
    EXPECT_EQ(list({{"path", "."}}), "(empty directory)");
}

TEST_F(LsToolTest, LimitAddsNotice) {
    for (const char* name : {"a", "b", "c"}) {
        ASSERT_TRUE(m_fs.writeFile(m_dir + "/" + name, "").has_value());
    }
    const auto result = m_tool.execute("c1", {{"limit", 2}}, nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<TextContent>(result->content[0]).text, "a\nb\n\n[2 entries limit reached. Use limit=4 for more]");
    EXPECT_EQ(result->details["entryLimitReached"], 2);
}

TEST_F(LsToolTest, ErrorsForMissingPathAndFile) {
    const auto missing = m_tool.execute("c1", {{"path", "nope"}}, nullptr, nullptr);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message, "Path not found: " + m_dir + "/nope");
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/f", "").has_value());
    const auto notDir = m_tool.execute("c1", {{"path", "f"}}, nullptr, nullptr);
    ASSERT_FALSE(notDir.has_value());
    EXPECT_EQ(notDir.error().message, "Not a directory: " + m_dir + "/f");
}
