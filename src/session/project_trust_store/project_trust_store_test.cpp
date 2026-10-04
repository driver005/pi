#include <gtest/gtest.h>

import std;
import pi.base.posix_file_lock;
import pi.base.posix_file_system;
import pi.session.project_trust_store;

class ProjectTrustStoreTest : public testing::Test {
protected:
    ProjectTrustStoreTest()
        : m_dir(std::string(std::getenv("TEST_TMPDIR")) + "/trust_" +
                testing::UnitTest::GetInstance()->current_test_info()->name()),
          m_store(m_dir + "/agent", m_files, m_lock) {
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir + "/proj/sub/deep");
        m_root = m_files.realPath(m_dir + "/proj");
    }

    std::string text() {
        return *m_files.readFile(m_dir + "/agent/trust.json");
    }

    std::string m_dir;
    std::string m_root;
    PosixFileSystem m_files;
    PosixFileLock m_lock;
    ProjectTrustStore m_store;
};

TEST_F(ProjectTrustStoreTest, UnknownDirectoryHasNoDecision) {
    const auto value = m_store.get(m_dir + "/proj");
    ASSERT_TRUE(value.has_value());
    EXPECT_FALSE(value->has_value());
}

TEST_F(ProjectTrustStoreTest, NearestAncestorDecisionApplies) {
    ASSERT_TRUE(m_store.set(m_dir + "/proj", true));
    ASSERT_TRUE(m_store.set(m_dir + "/proj/sub", false));
    const auto deep = m_store.getEntry(m_dir + "/proj/sub/deep");
    ASSERT_TRUE(deep.has_value() && deep->has_value());
    EXPECT_EQ((*deep)->path, m_root + "/sub");
    EXPECT_FALSE((*deep)->decision);
    const auto top = m_store.get(m_dir + "/proj");
    ASSERT_TRUE(top.has_value() && top->has_value());
    EXPECT_TRUE(**top);
}

TEST_F(ProjectTrustStoreTest, NullForgetsDecisionAndWritesSortedKeys) {
    ASSERT_TRUE(m_store.setMany({{m_dir + "/proj/sub", true}, {m_dir + "/proj", false}}));
    const std::string written = text();
    EXPECT_LT(written.find(m_root + "\""), written.find(m_root + "/sub\""));
    EXPECT_EQ(written.back(), '\n');
    ASSERT_TRUE(m_store.set(m_dir + "/proj/sub", std::nullopt));
    const auto value = m_store.get(m_dir + "/proj/sub");
    ASSERT_TRUE(value.has_value() && value->has_value());
    EXPECT_FALSE(**value);
}

TEST_F(ProjectTrustStoreTest, ReadsNullEntriesWrittenByTypeScript) {
    std::filesystem::create_directories(m_dir + "/agent");
    std::ofstream(m_dir + "/agent/trust.json")
        << "{\n  \"" << m_root << "\": true,\n  \"" << m_root << "/sub\": null\n}\n";
    const auto value = m_store.get(m_dir + "/proj/sub");
    ASSERT_TRUE(value.has_value() && value->has_value());
    EXPECT_TRUE(**value);
}

TEST_F(ProjectTrustStoreTest, InvalidFilesAreErrors) {
    std::filesystem::create_directories(m_dir + "/agent");
    std::ofstream(m_dir + "/agent/trust.json") << "[]";
    EXPECT_FALSE(m_store.get(m_dir + "/proj").has_value());
    std::ofstream(m_dir + "/agent/trust.json") << "{\"/x\": \"yes\"}";
    EXPECT_FALSE(m_store.get(m_dir + "/proj").has_value());
    std::ofstream(m_dir + "/agent/trust.json") << "{nope";
    EXPECT_FALSE(m_store.get(m_dir + "/proj").has_value());
}
