#include <gtest/gtest.h>

import std;
import pi.durable.sqlite_database;
import pi.support.sqlite_migrations;

class SqliteMigrationsTest : public ::testing::Test {
protected:
    SqliteMigrationsTest() : m_database(":memory:") {
        EXPECT_TRUE(m_database.open().has_value());
    }

    std::int64_t storedVersion() {
        auto row = m_database.get("SELECT version FROM durable_schema WHERE singleton = 1", {});
        return std::get<std::int64_t>((**row).at("version"));
    }

    SqliteDatabase m_database;
    SqliteMigrations m_migrations;
};

TEST_F(SqliteMigrationsTest, AppliesTheInitialSchemaOnceAndRecordsTheVersion) {
    ASSERT_TRUE(m_migrations.apply(m_database).has_value());
    EXPECT_EQ(storedVersion(), m_migrations.currentVersion());
    auto metadata = m_database.get("SELECT next_id, next_seq FROM durable_metadata WHERE singleton = 1", {});
    ASSERT_TRUE(metadata.has_value() && *metadata);
    EXPECT_EQ(std::get<std::string>((**metadata).at("next_id")), "2");
    EXPECT_EQ(std::get<std::int64_t>((**metadata).at("next_seq")), 1);
    // A second application changes nothing.
    ASSERT_TRUE(m_database.run("UPDATE durable_metadata SET next_seq = 5 WHERE singleton = 1", {}).has_value());
    ASSERT_TRUE(m_migrations.apply(m_database).has_value());
    EXPECT_EQ(std::get<std::int64_t>((**m_database.get("SELECT next_seq FROM durable_metadata", {})).at("next_seq")), 5);
}

TEST_F(SqliteMigrationsTest, CreatesEveryTableOfTheSharedSchema) {
    ASSERT_TRUE(m_migrations.apply(m_database).has_value());
    auto rows = m_database.all("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name", {});
    ASSERT_TRUE(rows.has_value());
    std::set<std::string> names;
    for (const SqlRow& row : *rows) {
        names.insert(std::get<std::string>(row.at("name")));
    }
    for (const char* table : {"conversations", "document_revisions", "documents", "durable_metadata", "durable_schema", "entries", "record_ids", "submissions", "tasks"}) {
        EXPECT_TRUE(names.contains(table)) << table;
    }
}

TEST_F(SqliteMigrationsTest, RefusesADatabaseNewerThanThisBuild) {
    ASSERT_TRUE(m_migrations.apply(m_database).has_value());
    ASSERT_TRUE(m_database.run("UPDATE durable_schema SET version = 99 WHERE singleton = 1", {}).has_value());
    auto refused = m_migrations.apply(m_database);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message.find("newer than supported version"), std::string::npos);
}

TEST_F(SqliteMigrationsTest, AFailingMigrationRollsBackEverything) {
    ASSERT_TRUE(m_database.exec("CREATE TABLE conversations (id INTEGER)").has_value());
    auto failed = m_migrations.apply(m_database);
    ASSERT_FALSE(failed.has_value());
    EXPECT_FALSE(m_database.get("SELECT 1 FROM sqlite_master WHERE name = 'durable_metadata'", {}).value().has_value());
    EXPECT_FALSE(m_database.get("SELECT 1 FROM sqlite_master WHERE name = 'durable_schema'", {}).value().has_value());
}
