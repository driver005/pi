#include <gtest/gtest.h>

#include <cstdlib>

import std;
import pi.durable.sqlite_database;

class SqliteDatabaseTest : public ::testing::Test {
protected:
    SqliteDatabase& open() {
        m_database = std::make_unique<SqliteDatabase>(":memory:");
        const auto opened = m_database->open();
        EXPECT_TRUE(opened.has_value()) << (opened ? "" : opened.error().message);
        EXPECT_TRUE(m_database->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, note TEXT) STRICT").has_value());
        return *m_database;
    }

    std::unique_ptr<SqliteDatabase> m_database;
};

TEST_F(SqliteDatabaseTest, RunsStatementsWithBindingsAndReadsRowsByColumnName) {
    SqliteDatabase& db = open();
    ASSERT_TRUE(db.run("INSERT INTO t (id, name, note) VALUES (?, ?, ?)", {std::int64_t(1), std::string("a\"b"), std::monostate{}}).has_value());
    ASSERT_TRUE(db.run("INSERT INTO t (id, name, note) VALUES (?, ?, ?)", {std::int64_t(2), std::string("c"), std::string("n")}).has_value());
    auto one = db.get("SELECT id, name, note FROM t WHERE id = ?", {std::int64_t(1)});
    ASSERT_TRUE(one.has_value() && *one);
    EXPECT_EQ(std::get<std::int64_t>((**one).at("id")), 1);
    EXPECT_EQ(std::get<std::string>((**one).at("name")), "a\"b");
    EXPECT_TRUE(std::holds_alternative<std::monostate>((**one).at("note")));
    EXPECT_FALSE(db.get("SELECT id FROM t WHERE id = ?", {std::int64_t(9)}).value());
    auto rows = db.all("SELECT id FROM t ORDER BY id DESC", {});
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2u);
    EXPECT_EQ(std::get<std::int64_t>((*rows)[0].at("id")), 2);
}

TEST_F(SqliteDatabaseTest, SqlErrorsAreReportedAndLeaveTheDatabaseUsable) {
    SqliteDatabase& db = open();
    auto bad = db.run("INSERT INTO missing VALUES (1)", {});
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, "storage_error");
    ASSERT_TRUE(db.run("INSERT INTO t (id, name, note) VALUES (1, 'x', 'y')", {}).has_value());
    auto duplicate = db.run("INSERT INTO t (id, name, note) VALUES (1, 'x', 'y')", {});
    EXPECT_FALSE(duplicate.has_value());
    EXPECT_TRUE(db.get("SELECT id FROM t", {}).value().has_value());
}

TEST_F(SqliteDatabaseTest, TransactionsCommitOnSuccessAndRollBackOnFailure) {
    SqliteDatabase& db = open();
    ASSERT_TRUE(db.transaction([&]() -> Result<void> {
        return db.run("INSERT INTO t (id, name, note) VALUES (1, 'kept', 'n')", {});
    }).has_value());
    auto failed = db.transaction([&]() -> Result<void> {
        if (auto inserted = db.run("INSERT INTO t (id, name, note) VALUES (2, 'lost', 'n')", {}); !inserted) {
            return inserted;
        }
        return std::unexpected(Error{"storage_error", "abandoned"});
    });
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "abandoned");
    EXPECT_EQ(db.all("SELECT id FROM t", {}).value().size(), 1u);
    auto nested = db.transaction([&]() -> Result<void> { return db.transaction([]() -> Result<void> { return {}; }); });
    ASSERT_FALSE(nested.has_value());
    EXPECT_NE(nested.error().message.find("nested"), std::string::npos);
}

TEST_F(SqliteDatabaseTest, FileDatabasesPersistAcrossReopeningAndCreateTheirDirectory) {
    const std::string path = std::string(std::getenv("TEST_TMPDIR")) + "/sqlite_database/nested/db.sqlite";
    std::filesystem::remove_all(std::filesystem::path(path).parent_path().parent_path());
    {
        SqliteDatabase db(path);
        ASSERT_TRUE(db.open().has_value());
        ASSERT_TRUE(db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY) STRICT; INSERT INTO t VALUES (7)").has_value());
        ASSERT_TRUE(db.close().has_value());
    }
    SqliteDatabase again(path);
    ASSERT_TRUE(again.open().has_value());
    auto row = again.get("SELECT id FROM t", {});
    ASSERT_TRUE(row.has_value() && *row);
    EXPECT_EQ(std::get<std::int64_t>((**row).at("id")), 7);
}

TEST_F(SqliteDatabaseTest, ClosedDatabasesRefuseEverything) {
    SqliteDatabase& db = open();
    ASSERT_TRUE(db.close().has_value());
    EXPECT_TRUE(db.close().has_value());
    EXPECT_FALSE(db.exec("SELECT 1").has_value());
    EXPECT_FALSE(db.get("SELECT 1", {}).has_value());
    EXPECT_FALSE(db.transaction([]() -> Result<void> { return {}; }).has_value());
}
