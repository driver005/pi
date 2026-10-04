#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.base.posix_file_system;
import pi.durable.jsonl_storage;
import pi.durable.memory_storage;
import pi.testing.fake_file_system;
import pi.testing.storage_conformance;

class JsonlBundle {
public:
    JsonlBundle(IFileSystem& files, const std::string& directory, bool fsync)
        : m_storage(m_memory, files, directory, JsonlStorageOptions{fsync}) {}

    Result<void> open() {
        return m_storage.open();
    }

    JsonlStorage& storage() {
        return m_storage;
    }

private:
    MemoryStorage m_memory;
    JsonlStorage m_storage;
};

class FakeBundle {
public:
    explicit FakeBundle(bool fsync) : m_bundle(m_files, "/data/store", fsync) {}

    FakeFileSystem m_files;
    JsonlBundle m_bundle;
};

class PosixBundle {
public:
    PosixBundle(const std::string& directory, bool fsync) : m_bundle(m_files, directory, fsync) {}

    PosixFileSystem m_files;
    JsonlBundle m_bundle;
};

class JsonlStorageTest : public ::testing::Test {
protected:
    /** A fresh JSONL storage over a shared fake file system; reopen() rebuilds it from the files. */
    void reopen(bool fsync = false) {
        m_bundle.reset();
        m_bundle = std::make_unique<JsonlBundle>(m_files, "/data/store", fsync);
        const auto opened = m_bundle->open();
        ASSERT_TRUE(opened) << opened.error().message;
    }

    Result<void> tryReopen() {
        m_bundle.reset();
        m_bundle = std::make_unique<JsonlBundle>(m_files, "/data/store", false);
        return m_bundle->open();
    }

    JsonlStorage& storage() {
        return m_bundle->storage();
    }

    std::int64_t commit(const std::vector<Json>& writes) {
        auto seq = storage().commit(writes);
        EXPECT_TRUE(seq) << (seq ? "" : seq.error().message);
        return seq ? *seq : 0;
    }

    Json conversation(std::int64_t id) const {
        return Json{{"type", "conversation"}, {"value", Json{{"id", id}}}};
    }

    Json entry(std::int64_t id, const std::string& kind = "message") const {
        return Json{{"type", "entry"}, {"value", Json{{"id", id}, {"conversationId", 1}, {"kind", kind}}}};
    }

    Json task(std::int64_t id, const std::string& status) const {
        Json state = status == "terminal"
                         ? Json{{"status", "terminal"}, {"outcome", Json{{"status", "completed"}, {"result", 1}}}}
                         : Json{{"status", status}, {"checkpoint", Json{{"phase", "ready"}}}};
        return Json{{"type", "task"},
                    {"value", Json{{"id", id},
                                   {"conversationId", 1},
                                   {"kind", "k"},
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

    Json baseDoc(std::int64_t id, const Json& value) const {
        return Json{{"type", "document.change"},
                    {"id", id},
                    {"content", Json{{"kind", "base"}, {"version", 1}, {"value", value}}}};
    }

    std::size_t lineCount(const std::string& file) {
        const auto content = m_files.readFile("/data/store/" + file);
        return content ? static_cast<std::size_t>(std::ranges::count(*content, '\n')) : 0;
    }

    FakeFileSystem m_files;
    std::unique_ptr<JsonlBundle> m_bundle;
};

TEST_F(JsonlStorageTest, PassesTheStorageConformanceSuite) {
    for (const bool fsync : {false, true}) {
        StorageConformance suite;
        const auto failures = suite.run([fsync] {
            auto bundle = std::make_shared<FakeBundle>(fsync);
            const auto opened = bundle->m_bundle.open();
            EXPECT_TRUE(opened);
            return std::shared_ptr<IStorage>(bundle, &bundle->m_bundle.storage());
        });
        for (const std::string& failure : failures) {
            ADD_FAILURE() << (fsync ? "[fsync] " : "") << failure;
        }
    }
}

TEST_F(JsonlStorageTest, PassesTheStorageConformanceSuiteOnTheRealFileSystem) {
    const std::string root = std::string(std::getenv("TEST_TMPDIR")) + "/jsonl_conformance";
    std::filesystem::remove_all(root);
    int counter = 0;
    StorageConformance suite;
    const auto failures = suite.run([&] {
        auto bundle = std::make_shared<PosixBundle>(root + "/case" + std::to_string(counter++), false);
        const auto opened = bundle->m_bundle.open();
        EXPECT_TRUE(opened);
        return std::shared_ptr<IStorage>(bundle, &bundle->m_bundle.storage());
    });
    for (const std::string& failure : failures) {
        ADD_FAILURE() << failure;
    }
}

TEST_F(JsonlStorageTest, ReopeningReproducesTheCommittedState) {
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

TEST_F(JsonlStorageTest, ATornFinalLineIsDroppedOnOpen) {
    reopen();
    commit({conversation(1)});
    commit({entry(2)});
    ASSERT_TRUE(m_files.appendFile("/data/store/main.jsonl", R"({"format":1,"type":"commit","seq":3,"wri)"));
    reopen();
    EXPECT_TRUE(storage().entry(2).value().has_value());
    EXPECT_EQ(commit({entry(3)}), 3);
    EXPECT_EQ(m_files.content("/data/store/main.jsonl").back(), '\n');
    reopen();
    EXPECT_TRUE(storage().entry(3).value().has_value());
}

TEST_F(JsonlStorageTest, UnconfirmedSidecarRecordsAreTruncated) {
    reopen();
    commit({conversation(1), createDoc(2, "rewindable", Json{{"n", 1}})});
    const std::string before = m_files.content("/data/store/doc-2.jsonl");
    const std::string orphan = R"({"format":1,"type":"record","seq":9,"ordinal":0,"payload":{"type":"document","id":2,"content":{"kind":"delta","version":1,"ops":[]}}})" "\n";
    ASSERT_TRUE(m_files.appendFile("/data/store/doc-2.jsonl", orphan));
    reopen();
    EXPECT_EQ(m_files.content("/data/store/doc-2.jsonl"), before);
    EXPECT_EQ(storage().document(2, DocumentPoint()).value()->deltasSinceBase, 0);
}

TEST_F(JsonlStorageTest, CorruptionIsReported) {
    reopen();
    commit({conversation(1)});
    commit({entry(2)});
    const std::string good = m_files.content("/data/store/main.jsonl");

    ASSERT_TRUE(m_files.writeFile("/data/store/main.jsonl", good + "{not json}\n"));
    auto opened = tryReopen();
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, "jsonl_corruption");

    const std::string first = good.substr(0, good.find('\n') + 1);
    ASSERT_TRUE(m_files.writeFile("/data/store/main.jsonl", first + first));
    opened = tryReopen();
    ASSERT_FALSE(opened);
    EXPECT_NE(opened.error().message.find("does not strictly increase"), std::string::npos);

    ASSERT_TRUE(m_files.writeFile("/data/store/main.jsonl", good + R"({"format":1,"type":"commit","seq":3,"writes":[{"type":"conversation","value":{"id":1}}]})" "\n"));
    opened = tryReopen();
    ASSERT_FALSE(opened);
    EXPECT_NE(opened.error().message.find("Invalid committed state at sequence 3"), std::string::npos);

    ASSERT_TRUE(m_files.writeFile("/data/store/main.jsonl", good + "\xff\xfe\n"));
    opened = tryReopen();
    ASSERT_FALSE(opened);
    EXPECT_NE(opened.error().message.find("Invalid UTF-8"), std::string::npos);
}

TEST_F(JsonlStorageTest, AMissingConfirmedSidecarRecordIsCorruption) {
    reopen();
    commit({conversation(1), createDoc(2, "rewindable", Json{{"n", 1}})});
    ASSERT_TRUE(m_files.removeFile("/data/store/doc-2.jsonl"));
    const auto opened = tryReopen();
    ASSERT_FALSE(opened);
    EXPECT_NE(opened.error().message.find("Missing confirmed sidecar record doc-2.jsonl"), std::string::npos);
}

TEST_F(JsonlStorageTest, CurrentOnlyDocumentsKeepOnlyTheirLatestBase) {
    reopen();
    commit({conversation(1), createDoc(2, "latest", Json{{"n", 1}})});
    commit({deltaDoc(2, "n", 2)});
    commit({deltaDoc(2, "n", 3)});
    EXPECT_EQ(lineCount("doc-2.jsonl"), 3u);
    commit({baseDoc(2, Json{{"n", 10}})});
    EXPECT_EQ(lineCount("doc-2.jsonl"), 1u);
    commit({deltaDoc(2, "n", 11)});
    reopen();
    EXPECT_EQ(storage().document(2, DocumentPoint()).value()->value, (Json{{"n", 11}}));
}

TEST_F(JsonlStorageTest, RewindableDocumentsKeepEveryRevision) {
    reopen();
    commit({conversation(1), createDoc(2, "rewindable", Json{{"n", 1}})});
    commit({deltaDoc(2, "n", 2)});
    commit({baseDoc(2, Json{{"n", 10}})});
    EXPECT_EQ(lineCount("doc-2.jsonl"), 3u);
}

TEST_F(JsonlStorageTest, RetiredCurrentOnlyDocumentsAndTerminalTasksLoseTheirSidecars) {
    reopen();
    commit({conversation(1), createDoc(2, "latest", Json{{"n", 1}}), task(3, "pending")});
    EXPECT_TRUE(m_files.exists("/data/store/doc-2.jsonl"));
    EXPECT_TRUE(m_files.exists("/data/store/task-3.jsonl"));
    commit({Json{{"type", "document.retire"}, {"id", 2}}, task(3, "terminal")});
    EXPECT_FALSE(m_files.exists("/data/store/doc-2.jsonl"));
    EXPECT_FALSE(m_files.exists("/data/store/task-3.jsonl"));
    reopen();
    EXPECT_EQ((*storage().task(3).value())["state"]["status"], "terminal");
    EXPECT_FALSE(storage().document(2, DocumentPoint()).value().has_value());
}

TEST_F(JsonlStorageTest, ReclaimedSidecarsAreCleanedUpWhenReopening) {
    reopen();
    commit({conversation(1), createDoc(2, "latest", Json{{"n", 1}})});
    ASSERT_TRUE(m_files.writeFile("/data/store/doc-2.jsonl.reclaim", "partial"));
    reopen();
    EXPECT_FALSE(m_files.exists("/data/store/doc-2.jsonl.reclaim"));
    EXPECT_EQ(storage().document(2, DocumentPoint()).value()->value, (Json{{"n", 1}}));
}

TEST_F(JsonlStorageTest, AFailedWritePoisonsTheStorageUntilItIsReopened) {
    reopen();
    commit({conversation(1)});
    m_files.failAppends(true);
    const auto failed = storage().commit({entry(2)});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, "jsonl_poisoned");
    m_files.failAppends(false);
    EXPECT_EQ(storage().mintId().error().code, "jsonl_poisoned");
    EXPECT_EQ(storage().commit({entry(3)}).error().code, "jsonl_poisoned");
    EXPECT_EQ(storage().conversation(1).error().code, "jsonl_poisoned");
    reopen();
    EXPECT_FALSE(storage().entry(2).value().has_value());
    commit({entry(2)});
    EXPECT_TRUE(storage().entry(2).value().has_value());
}

TEST_F(JsonlStorageTest, RejectedCommitsWriteNothing) {
    reopen();
    commit({conversation(1)});
    const std::string before = m_files.content("/data/store/main.jsonl");
    EXPECT_FALSE(storage().commit({conversation(1)}));
    EXPECT_EQ(m_files.content("/data/store/main.jsonl"), before);
    EXPECT_FALSE(storage().commit({createDoc(2, "latest", Json::object()), createDoc(2, "latest", Json::object())}));
    EXPECT_FALSE(m_files.exists("/data/store/doc-2.jsonl"));
}

TEST_F(JsonlStorageTest, ClosedStoragesRefuseEverything) {
    reopen();
    commit({conversation(1)});
    ASSERT_TRUE(storage().close());
    ASSERT_TRUE(storage().close());
    EXPECT_FALSE(storage().commit({entry(2)}));
    EXPECT_FALSE(storage().conversation(1));
}

TEST_F(JsonlStorageTest, EmptyDirectoriesOpenAsEmptyStorages) {
    reopen();
    EXPECT_EQ(storage().mintId().value(), 2);
    EXPECT_FALSE(storage().conversation(1).value().has_value());
    EXPECT_TRUE(m_files.exists("/data/store"));
}
