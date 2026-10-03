#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.durable.sqlite_database;
import pi.durable.sqlite_storage;
import pi.testing.storage_conformance;

class SqliteBundle {
public:
    explicit SqliteBundle(const std::string& path)
        : m_database(std::make_shared<SqliteDatabase>(path)),
          m_storage(m_database) {}

    Result<void> open() {
        if (auto opened = m_database->open(); !opened) {
            return opened;
        }
        return m_storage.open();
    }

    SqliteStorage& storage() {
        return m_storage;
    }

    SqliteDatabase& database() {
        return *m_database;
    }

private:
    std::shared_ptr<SqliteDatabase> m_database;
    SqliteStorage m_storage;
};

class SqliteStorageTest : public ::testing::Test {
protected:
    std::string path() {
        return std::string(std::getenv("TEST_TMPDIR")) + "/sqlite_storage/" + ::testing::UnitTest::GetInstance()->current_test_info()->name() + "/db.sqlite";
    }

    void reopen() {
        m_bundle.reset();
        m_bundle = std::make_unique<SqliteBundle>(path());
        const auto opened = m_bundle->open();
        ASSERT_TRUE(opened.has_value()) << opened.error().message;
    }

    void SetUp() override {
        std::filesystem::remove_all(std::filesystem::path(path()).parent_path());
    }

    SqliteStorage& storage() {
        return m_bundle->storage();
    }

    std::int64_t commit(const std::vector<Json>& writes) {
        auto seq = storage().commit(writes);
        EXPECT_TRUE(seq.has_value()) << (seq ? "" : seq.error().message);
        return seq ? *seq : 0;
    }

    Json conversation(std::int64_t id) const {
        return Json{{"type", "conversation"}, {"value", Json{{"id", id}}}};
    }

    Json entry(std::int64_t id, const std::string& kind = "message") const {
        return Json{{"type", "entry"}, {"value", Json{{"id", id}, {"conversationId", 1}, {"kind", kind}}}};
    }

    Json task(std::int64_t id, const std::string& status, const std::string& kind = "k") const {
        Json state = status == "terminal"
                         ? Json{{"status", "terminal"}, {"outcome", Json{{"status", "completed"}, {"result", 1}}}}
                         : Json{{"status", status}, {"checkpoint", Json{{"phase", "ready"}}}};
        return Json{{"type", "task"},
                    {"value", Json{{"id", id},
                                   {"conversationId", 1},
                                   {"kind", kind},
                                   {"version", 1},
                                   {"input", Json::object()},
                                   {"state", state},
                                   {"background", false},
                                   {"abortRequested", false}}}};
    }

    Json createDoc(std::int64_t id, const std::string& history, const Json& value) const {
        Json record = {{"id", id}, {"kind", "doc"}, {"scope", Json{{"kind", "conversation"}, {"conversationId", 1}}}};
        record["history"] = history;
        record["fork"] = history == "latest" ? "current" : "asOf";
        return Json{{"type", "document.create"},
                    {"record", record},
                    {"content", Json{{"kind", "base"}, {"version", 1}, {"value", value}}}};
    }

    Json deltaDoc(std::int64_t id, const std::string& key, const Json& value) const {
        return Json{{"type", "document.change"},
                    {"id", id},
                    {"content", Json{{"kind", "delta"}, {"version", 1}, {"ops", Json::array({Json::array({"s", Json::array({key}), value})})}}}};
    }

    std::unique_ptr<SqliteBundle> m_bundle;
};

TEST_F(SqliteStorageTest, PassesTheStorageConformanceSuiteInMemory) {
    StorageConformance suite;
    const auto failures = suite.run([] {
        auto bundle = std::make_shared<SqliteBundle>(":memory:");
        const auto opened = bundle->open();
        EXPECT_TRUE(opened.has_value());
        return std::shared_ptr<IStorage>(bundle, &bundle->storage());
    });
    for (const std::string& failure : failures) {
        ADD_FAILURE() << failure;
    }
}

TEST_F(SqliteStorageTest, PassesTheStorageConformanceSuiteOnFiles) {
    const std::string root = std::string(std::getenv("TEST_TMPDIR")) + "/sqlite_conformance";
    std::filesystem::remove_all(root);
    int counter = 0;
    StorageConformance suite;
    const auto failures = suite.run([&] {
        auto bundle = std::make_shared<SqliteBundle>(root + "/case" + std::to_string(counter++) + "/db.sqlite");
        const auto opened = bundle->open();
        EXPECT_TRUE(opened.has_value());
        return std::shared_ptr<IStorage>(bundle, &bundle->storage());
    });
    for (const std::string& failure : failures) {
        ADD_FAILURE() << failure;
    }
}

TEST_F(SqliteStorageTest, ReopeningReproducesTheCommittedState) {
    reopen();
    commit({conversation(1)});
    commit({entry(2), task(3, "pending"), createDoc(4, "rewindable", Json{{"n", 1}})});
    const std::int64_t last = commit({deltaDoc(4, "n", 2), task(3, "running"), entry(5, "later")});
    reopen();
    EXPECT_EQ(storage().mintId().value(), 6);
    EXPECT_EQ(storage().entry(5).value()->commitSeq, last);
    EXPECT_EQ((*storage().task(3).value())["state"]["status"], "running");
    EXPECT_EQ(storage().document(4, DocumentPoint()).value()->value, (Json{{"n", 2}}));
    EXPECT_EQ(storage().document(4, DocumentPoint()).value()->deltasSinceBase, 1);
    EXPECT_EQ(commit({entry(6)}), last + 1);
}

TEST_F(SqliteStorageTest, IdentityStringsAreStoredJsonEncodedLikeTheTypeScriptImplementation) {
    reopen();
    commit({conversation(1)});
    commit({task(2, "pending", "a\"b")});
    auto row = m_bundle->database().get("SELECT kind, status, abort_requested FROM tasks WHERE id = 2", {});
    ASSERT_TRUE(row.has_value() && *row);
    EXPECT_EQ(std::get<std::string>((**row).at("kind")), "\"a\\\"b\"");
    EXPECT_EQ(std::get<std::string>((**row).at("status")), "pending");
    EXPECT_EQ(std::get<std::int64_t>((**row).at("abort_requested")), 0);
    // The scan filter encodes its argument the same way.
    TaskQuery query;
    query.kind = "a\"b";
    auto page = storage().scanTasks(query, 10, std::nullopt);
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(page->items.size(), 1u);
}

TEST_F(SqliteStorageTest, RejectedCommitsWriteNothing) {
    reopen();
    commit({conversation(1)});
    auto rejected = storage().commit({entry(2), conversation(1)});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_NE(rejected.error().message.find("already belongs to conversation"), std::string::npos);
    EXPECT_FALSE(storage().entry(2).value().has_value());
    EXPECT_EQ(commit({entry(2)}), 2);
}

TEST_F(SqliteStorageTest, ANewerSchemaRefusesToOpen) {
    reopen();
    commit({conversation(1)});
    ASSERT_TRUE(m_bundle->database().run("UPDATE durable_schema SET version = 99 WHERE singleton = 1", {}).has_value());
    ASSERT_TRUE(storage().close().has_value());
    m_bundle = std::make_unique<SqliteBundle>(path());
    auto opened = m_bundle->open();
    ASSERT_FALSE(opened.has_value());
    EXPECT_NE(opened.error().message.find("newer than supported version"), std::string::npos);
    // The failed open released the database, so the storage refuses work.
    EXPECT_FALSE(storage().mintId().has_value() && storage().conversation(1).has_value());
}

TEST_F(SqliteStorageTest, ClosedStoragesRefuseEverything) {
    reopen();
    commit({conversation(1)});
    ASSERT_TRUE(storage().close().has_value());
    EXPECT_FALSE(storage().commit({entry(2)}).has_value());
    EXPECT_FALSE(storage().mintId().has_value());
    EXPECT_FALSE(storage().conversation(1).has_value());
    EXPECT_FALSE(storage().scanTasks(TaskQuery{}, 10, std::nullopt).has_value());
}
