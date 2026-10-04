#include <gtest/gtest.h>
#include <cstdlib>

import std;
import pi.ai.file_models_store;
import pi.base.posix_file_lock;
import pi.base.posix_file_system;

class FileModelsStoreTest : public testing::Test {
protected:
    FileModelsStoreTest()
        : m_path(std::string(std::getenv("TEST_TMPDIR")) + "/models_store_" +
                 testing::UnitTest::GetInstance()->current_test_info()->name() + "/agent/models-store.json"),
          m_store(m_path, m_files, m_lock) {
        std::filesystem::remove_all(std::filesystem::path(m_path).parent_path().parent_path());
    }

    std::string m_path;
    PosixFileSystem m_files;
    PosixFileLock m_lock;
    FileModelsStore m_store;
};

TEST_F(FileModelsStoreTest, MissingEntryReadsAsNullopt) {
    const auto entry = m_store.read("anthropic");
    ASSERT_TRUE(entry.has_value());
    EXPECT_FALSE(entry->has_value());
}

TEST_F(FileModelsStoreTest, WriteReadRemoveRoundTrip) {
    ModelsStoreEntry entry;
    entry.models = Json::parse(R"([{"id":"a"},{"id":"b"}])");
    entry.lastModified = 1700000000000;
    entry.checkedAt = 1700000001000;
    entry.etag = "\"abc\"";
    ASSERT_TRUE(m_store.write("anthropic", entry).has_value());
    ModelsStoreEntry other;
    ASSERT_TRUE(m_store.write("openai", other).has_value());
    const auto read = m_store.read("anthropic");
    ASSERT_TRUE(read->has_value());
    EXPECT_EQ((*read)->models.size(), 2U);
    EXPECT_EQ((*read)->lastModified, 1700000000000);
    EXPECT_EQ((*read)->etag, "\"abc\"");
    ASSERT_TRUE(m_store.remove("anthropic").has_value());
    EXPECT_FALSE(m_store.read("anthropic")->has_value());
    EXPECT_TRUE(m_store.read("openai")->has_value());
}

TEST_F(FileModelsStoreTest, CorruptFileIsAnError) {
    std::filesystem::create_directories(std::filesystem::path(m_path).parent_path());
    ASSERT_TRUE(m_files.writeFile(m_path, "[1,2]").has_value());
    EXPECT_FALSE(m_store.read("x").has_value());
}
