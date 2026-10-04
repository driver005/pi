#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.base.posix_file_system;
import pi.tools.file_mutation_queue;
import pi.tools.write_tool;

class WriteToolTest : public testing::Test {
protected:
    PosixFileSystem m_fs;
    FileMutationQueue m_queue{m_fs};
    std::string m_dir = [] {
        const std::string dir = std::string(std::getenv("TEST_TMPDIR")) + "/write_" +
                                testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }();
    WriteTool m_tool{m_fs, m_queue, m_dir};
};

TEST_F(WriteToolTest, CreatesFileAndParentDirectories) {
    const auto result = m_tool.execute("c1", {{"path", "deep/er/a.txt"}, {"content", "hello"}}, nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<TextContent>(result->content[0]).text, "Successfully wrote to deep/er/a.txt");
    EXPECT_EQ(m_fs.readFile(m_dir + "/deep/er/a.txt").value(), "hello");
}

TEST_F(WriteToolTest, OverwritesExistingFile) {
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/a.txt", "old old old").has_value());
    ASSERT_TRUE(m_tool.execute("c1", {{"path", "a.txt"}, {"content", "new"}}, nullptr, nullptr).has_value());
    EXPECT_EQ(m_fs.readFile(m_dir + "/a.txt").value(), "new");
}

TEST_F(WriteToolTest, AbortedBeforeStartWritesNothing) {
    auto signal = std::make_shared<AbortSignal>();
    signal->abort();
    const auto result = m_tool.execute("c1", {{"path", "a.txt"}, {"content", "x"}}, signal, nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(m_fs.exists(m_dir + "/a.txt"));
}

TEST_F(WriteToolTest, WriteIntoDirectoryPathFails) {
    ASSERT_TRUE(m_fs.createDirectories(m_dir + "/d").has_value());
    const auto result = m_tool.execute("c1", {{"path", "d"}, {"content", "x"}}, nullptr, nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "EISDIR");
}

TEST_F(WriteToolTest, ConcurrentWritesToSameFileStayWhole) {
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, i] {
            m_tool.execute("c", {{"path", "race.txt"}, {"content", std::string(50000, static_cast<char>('a' + i))}},
                           nullptr, nullptr);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const std::string content = m_fs.readFile(m_dir + "/race.txt").value();
    ASSERT_EQ(content.size(), 50000U);
    EXPECT_EQ(content.find_first_not_of(content[0]), std::string::npos);
}
