#include <gtest/gtest.h>

#include <sys/stat.h>
#include <cstdlib>

import std;
import pi.base.posix_file_system;

class PosixFileSystemTest : public testing::Test {
protected:
    PosixFileSystemTest() {
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/fs_" +
                testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir);
    }

    PosixFileSystem m_fs;
    std::string m_dir;
};

TEST_F(PosixFileSystemTest, WriteReadAppendRoundTrip) {
    const std::string path = m_dir + "/a.txt";
    ASSERT_TRUE(m_fs.writeFile(path, "hello").has_value());
    ASSERT_TRUE(m_fs.appendFile(path, " world").has_value());
    EXPECT_EQ(m_fs.readFile(path).value(), "hello world");
    ASSERT_TRUE(m_fs.writeFile(path, "x").has_value());
    EXPECT_EQ(m_fs.readFile(path).value(), "x");
}

TEST_F(PosixFileSystemTest, LargeAndBinaryContent) {
    std::string data;
    for (int i = 0; i < 300000; ++i) {
        data.push_back(static_cast<char>(i % 256));
    }
    const std::string path = m_dir + "/bin";
    ASSERT_TRUE(m_fs.writeFile(path, data).has_value());
    EXPECT_EQ(m_fs.readFile(path).value(), data);
}

TEST_F(PosixFileSystemTest, MissingFileReportsErrnoName) {
    const auto result = m_fs.readFile(m_dir + "/missing");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "ENOENT");
    EXPECT_NE(result.error().message.find("no such file or directory"), std::string::npos);
    EXPECT_FALSE(m_fs.exists(m_dir + "/missing"));
}

TEST_F(PosixFileSystemTest, DirectoriesStatAndListing) {
    ASSERT_TRUE(m_fs.createDirectories(m_dir + "/x/y/z").has_value());
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/x/f.txt", "12345").has_value());
    const auto dirStat = m_fs.stat(m_dir + "/x");
    ASSERT_TRUE(dirStat.has_value());
    EXPECT_TRUE(dirStat->isDirectory);
    const auto fileStat = m_fs.stat(m_dir + "/x/f.txt");
    EXPECT_TRUE(fileStat->isFile);
    EXPECT_EQ(fileStat->size, 5U);
    auto names = m_fs.listDirectory(m_dir + "/x").value();
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"f.txt", "y"}));
    EXPECT_EQ(m_fs.readFile(m_dir + "/x").error().code, "EISDIR");
    EXPECT_FALSE(m_fs.listDirectory(m_dir + "/nope").has_value());
}

TEST_F(PosixFileSystemTest, RenameRemoveAndRealPath) {
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/a", "1").has_value());
    ASSERT_TRUE(m_fs.renameFile(m_dir + "/a", m_dir + "/b").has_value());
    EXPECT_FALSE(m_fs.exists(m_dir + "/a"));
    std::filesystem::create_symlink(m_dir + "/b", m_dir + "/link");
    EXPECT_EQ(m_fs.realPath(m_dir + "/link"), m_fs.realPath(m_dir + "/b"));
    EXPECT_EQ(m_fs.realPath(m_dir + "/missing"), m_dir + "/missing");
    ASSERT_TRUE(m_fs.removeFile(m_dir + "/b").has_value());
    EXPECT_FALSE(m_fs.removeFile(m_dir + "/b").has_value());
}

TEST_F(PosixFileSystemTest, PermissionChecksAndHome) {
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/p", "x").has_value());
    EXPECT_TRUE(m_fs.isReadable(m_dir + "/p"));
    EXPECT_TRUE(m_fs.isWritable(m_dir + "/p"));
    EXPECT_FALSE(m_fs.homeDirectory().empty());
}

TEST_F(PosixFileSystemTest, PrivateFilesAndDirectoriesUseOwnerOnlyModes) {
    const std::string nested = m_dir + "/a/b";
    ASSERT_TRUE(m_fs.createPrivateDirectories(nested).has_value());
    struct stat info {};
    ASSERT_EQ(stat(nested.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0700U);
    const std::string file = nested + "/auth.json";
    ASSERT_TRUE(m_fs.writeFilePrivate(file, "{}").has_value());
    ASSERT_EQ(stat(file.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600U);
    // Existing files keep their mode when rewritten.
    chmod(file.c_str(), 0640);
    ASSERT_TRUE(m_fs.writeFilePrivate(file, "{\"a\":1}").has_value());
    ASSERT_EQ(stat(file.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0640U);
}

TEST_F(PosixFileSystemTest, ReadFilePrefixStopsAtLimit) {
    const std::string path = m_dir + "/prefix.txt";
    ASSERT_TRUE(m_fs.writeFile(path, "0123456789").has_value());
    EXPECT_EQ(m_fs.readFilePrefix(path, 4).value(), "0123");
    EXPECT_EQ(m_fs.readFilePrefix(path, 100).value(), "0123456789");
    EXPECT_EQ(m_fs.readFilePrefix(m_dir + "/missing", 4).error().code, "ENOENT");
}
