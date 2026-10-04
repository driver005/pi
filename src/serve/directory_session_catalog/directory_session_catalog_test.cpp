#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.serve.directory_session_catalog;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.sequential_id_generator;

class DirectorySessionCatalogTest : public ::testing::Test {
protected:
    std::vector<std::string> ids() {
        std::vector<std::string> result;
        for (const SessionRecord& record : *m_catalog.list()) {
            result.push_back(record.id);
        }
        return result;
    }

    FakeFileSystem m_files;
    FixedClock m_clock{5000};
    SequentialIdGenerator m_ids{"session-"};
    DirectorySessionCatalog m_catalog{m_files, m_clock, m_ids, "/srv/sessions", "/work/project"};
};

TEST_F(DirectorySessionCatalogTest, StartsEmptyWithoutADirectory) {
    const auto records = m_catalog.list();
    ASSERT_TRUE(records);
    EXPECT_TRUE(records->empty());
}

TEST_F(DirectorySessionCatalogTest, CreateWritesTheTsMetadataFormat) {
    const auto record = m_catalog.create(std::string("alpha"));
    ASSERT_TRUE(record);
    EXPECT_EQ(record->id, "alpha");
    EXPECT_EQ(record->createdAt, 5000);
    EXPECT_EQ(record->cwd, "/work/project");
    EXPECT_EQ(record->directory, "/srv/sessions/alpha");
    EXPECT_EQ(m_files.content("/srv/sessions/alpha/meta.json"), "{\n\t\"createdAt\": 5000,\n\t\"cwd\": \"/work/project\"\n}\n");
}

TEST_F(DirectorySessionCatalogTest, CreateGeneratesIdsAndRefusesBadOrDuplicateOnes) {
    EXPECT_EQ(m_catalog.create(std::nullopt)->id, "session-1");
    EXPECT_EQ(m_catalog.create(std::string("session-1")).error().code, "session_exists");
    EXPECT_EQ(m_catalog.create(std::string("../escape")).error().code, "invalid_request");
    EXPECT_EQ(m_catalog.create(std::string("")).error().code, "invalid_request");
    EXPECT_EQ(m_catalog.create(std::string("-leading")).error().code, "invalid_request");
    EXPECT_EQ(m_catalog.create(std::string(129, 'a')).error().code, "invalid_request");
    EXPECT_TRUE(m_catalog.create(std::string("A.b_c-1")));
}

TEST_F(DirectorySessionCatalogTest, ListIsNewestFirstAndSkipsBrokenEntries) {
    m_catalog.create(std::string("old"));
    m_clock.advance(1000);
    m_catalog.create(std::string("new"));
    m_files.createDirectories("/srv/sessions/not-a-session");
    m_files.createDirectories("/srv/sessions/broken");
    m_files.writeFile("/srv/sessions/broken/meta.json", "{not json");
    m_files.createDirectories("/srv/sessions/partial");
    m_files.writeFile("/srv/sessions/partial/meta.json", "{\"createdAt\": 1}");
    EXPECT_EQ(ids(), (std::vector<std::string>{"new", "old"}));
}

TEST_F(DirectorySessionCatalogTest, ResolveFindsExactIdsAndUniquePrefixes) {
    m_catalog.create(std::string("alpha-1"));
    m_catalog.create(std::string("alpha-2"));
    m_catalog.create(std::string("beta"));
    EXPECT_EQ(m_catalog.resolve("alpha-1")->id, "alpha-1");
    EXPECT_EQ(m_catalog.resolve("be")->id, "beta");
    EXPECT_EQ(m_catalog.resolve("alpha").error().code, "session_ambiguous");
    EXPECT_EQ(m_catalog.resolve("zzz").error().code, "session_not_found");
    EXPECT_EQ(m_catalog.resolve("").error().code, "session_not_found");
}

TEST_F(DirectorySessionCatalogTest, RemoveDeletesTheSessionDirectory) {
    m_catalog.create(std::string("gone"));
    m_files.writeFile("/srv/sessions/gone/session.jsonl", "{}");
    ASSERT_TRUE(m_catalog.remove("gone"));
    EXPECT_FALSE(m_files.exists("/srv/sessions/gone"));
    EXPECT_FALSE(m_files.exists("/srv/sessions/gone/session.jsonl"));
    EXPECT_TRUE(ids().empty());
    EXPECT_EQ(m_catalog.remove("gone").error().code, "session_not_found");
}
