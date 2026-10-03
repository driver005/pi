#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.transaction;

class TestHost : public ITransactionHost {
public:
    explicit TestHost(IStorage& storage) : m_storage(storage) {}

    IStorage& storage() override {
        return m_storage;
    }

    std::shared_ptr<LoadedDocument> cached(const std::string& addressId) override {
        auto found = m_cache.find(addressId);
        return found == m_cache.end() ? nullptr : found->second;
    }

    Result<std::shared_ptr<LoadedDocument>> load(const DocDefinition& definition, const std::string& addressId,
                                                 const DocumentAddress& address) override {
        if (auto hit = cached(addressId); hit && hit->valueVersion == definition.version) {
            return hit;
        }
        m_cache.erase(addressId);
        auto record = m_storage.findDocument(address, DocumentPoint{true, 0});
        if (!record) {
            return std::unexpected(record.error());
        }
        if (!*record) {
            return std::shared_ptr<LoadedDocument>();
        }
        auto stored = m_storage.document((*record)->at("id").get<std::int64_t>(), DocumentPoint{true, 0});
        if (!stored || !*stored) {
            return std::unexpected(Error{"durable_error", "cannot read document"});
        }
        auto value = m_resolver.materialize(definition, (*stored)->record, (*stored)->version, (*stored)->value);
        if (!value) {
            return std::unexpected(value.error());
        }
        auto loaded = std::make_shared<LoadedDocument>();
        loaded->addressId = addressId;
        loaded->record = (*stored)->record;
        loaded->storedVersion = (*stored)->version;
        loaded->valueVersion = definition.version;
        loaded->deltasSinceBase = (*stored)->deltasSinceBase;
        loaded->value = *value;
        m_cache[addressId] = loaded;
        return loaded;
    }

    void install(std::shared_ptr<LoadedDocument> document) override {
        m_cache[document->addressId] = document;
    }

    void evict(const std::string& addressId, std::int64_t recordId) override {
        auto found = m_cache.find(addressId);
        if (found != m_cache.end() && found->second->record.at("id").get<std::int64_t>() == recordId) {
            m_cache.erase(found);
        }
    }

private:
    IStorage& m_storage;
    DocumentResolver m_resolver;
    std::map<std::string, std::shared_ptr<LoadedDocument>> m_cache;
};

class TransactionTest : public ::testing::Test {
protected:
    TransactionTest() : m_host(m_storage) {}

    /** Runs the callback in a transaction and commits it like a session; returns the publications. */
    std::vector<Json> commit(const std::function<Result<void>(Transaction&)>& body, TransactionScope scope = {}) {
        Transaction tx(m_host, scope, nullptr);
        auto result = body(tx);
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
        auto writes = tx.settleSuccess();
        EXPECT_TRUE(writes.has_value()) << (writes ? "" : writes.error().message);
        if (!writes || writes->empty()) {
            return {};
        }
        auto seq = m_storage.commit(*writes);
        EXPECT_TRUE(seq.has_value());
        return tx.adopt(*seq);
    }

    Result<void> failing(const std::function<Result<void>(Transaction&)>& body) {
        Transaction tx(m_host, {}, nullptr);
        if (auto result = body(tx); !result) {
            return result;
        }
        auto writes = tx.settleSuccess();
        if (!writes) {
            return std::unexpected(writes.error());
        }
        return {};
    }

    DocDefinition counter(std::int64_t version = 1) {
        DocDefinition def;
        def.kind = "counter";
        def.version = version;
        def.scope = "conversation";
        def.history = "latest";
        def.fork = "current";
        def.initial = [](const Json&) { return Json::object({{"count", 0}}); };
        return def;
    }

    DocAddressArgs on(std::int64_t owner) {
        DocAddressArgs args;
        args.owner = owner;
        return args;
    }

    std::int64_t makeConversation() {
        std::int64_t id = 0;
        commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
            if (!created) {
                return std::unexpected(created.error());
            }
            id = created->at("id").get<std::int64_t>();
            return {};
        });
        return id;
    }

    MemoryStorage m_storage;
    TestHost m_host;
};

TEST_F(TransactionTest, CommitsConversationAndEntryWithHeadAndTaskAttribution) {
    std::int64_t conversationId = makeConversation();
    Json entry;
    commit(
        [&](Transaction& tx) -> Result<void> {
            auto appended = tx.appendEntry(conversationId, Json::object({{"kind", "message"}, {"head", "self"}}));
            if (!appended) {
                return std::unexpected(appended.error());
            }
            entry = *appended;
            return {};
        },
        TransactionScope{std::nullopt, 77});
    EXPECT_EQ(entry.at("head"), entry.at("id"));
    EXPECT_EQ(entry.at("byTaskId"), 77);
    auto stored = m_storage.entry(entry.at("id").get<std::int64_t>());
    ASSERT_TRUE(stored.has_value());
    ASSERT_TRUE(stored->has_value());
}

TEST_F(TransactionTest, ReadsAreRefusedAfterTheFirstTableWrite) {
    std::int64_t conversationId = makeConversation();
    Transaction tx(m_host, {}, nullptr);
    ASSERT_TRUE(tx.appendEntry(conversationId, Json::object({{"kind", "message"}})).has_value());
    auto read = tx.conversation(conversationId);
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().code, "read_after_write");
}

TEST_F(TransactionTest, CreatesThenChangesADocumentAsADelta) {
    std::int64_t conversationId = makeConversation();
    auto first = commit([&](Transaction& tx) -> Result<void> {
        auto draft = tx.doc(counter(), on(conversationId));
        if (!draft) {
            return std::unexpected(draft.error());
        }
        (**draft)["count"] = 1;
        return {};
    });
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].at("value").at("count"), 1);
    EXPECT_EQ(first[0].at("ops").size(), 0u);

    auto second = commit([&](Transaction& tx) -> Result<void> {
        auto draft = tx.doc(counter(), on(conversationId));
        if (!draft) {
            return std::unexpected(draft.error());
        }
        (**draft)["count"] = 2;
        return {};
    });
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].at("value").at("count"), 2);
    EXPECT_EQ(second[0].at("ops").size(), 1u);
    auto cached = m_host.cached(R"(["counter","conversation",)" + std::to_string(conversationId) + R"(,null])");
    ASSERT_TRUE(cached != nullptr);
    EXPECT_EQ(cached->value.at("count"), 2);
    EXPECT_EQ(cached->deltasSinceBase, 1);
}

TEST_F(TransactionTest, UnchangedDocumentWritesNothing) {
    std::int64_t conversationId = makeConversation();
    commit([&](Transaction& tx) -> Result<void> {
        return tx.doc(counter(), on(conversationId)).transform([](Json*) {});
    });
    auto again = commit([&](Transaction& tx) -> Result<void> {
        return tx.doc(counter(), on(conversationId)).transform([](Json*) {});
    });
    EXPECT_TRUE(again.empty());
}

TEST_F(TransactionTest, CheckpointPredicateStoresABase) {
    std::int64_t conversationId = makeConversation();
    DocDefinition def = counter();
    def.checkpointWhen = [](const Json&, const Json&, std::int64_t deltas) { return deltas >= 1; };
    for (int step = 1; step <= 3; ++step) {
        commit([&](Transaction& tx) -> Result<void> {
            auto draft = tx.doc(def, on(conversationId));
            if (!draft) {
                return std::unexpected(draft.error());
            }
            (**draft)["count"] = step;
            return {};
        });
    }
    auto cached = m_host.cached(R"(["counter","conversation",)" + std::to_string(conversationId) + R"(,null])");
    ASSERT_TRUE(cached != nullptr);
    // Step 1 creates a base, step 2 stores a delta, step 3 meets the predicate and stores a base again.
    EXPECT_EQ(cached->deltasSinceBase, 0);
    EXPECT_EQ(cached->value.at("count"), 3);
}

TEST_F(TransactionTest, RetireRemovesTheDocumentAndANewIncarnationStartsFresh) {
    std::int64_t conversationId = makeConversation();
    commit([&](Transaction& tx) -> Result<void> {
        auto draft = tx.doc(counter(), on(conversationId));
        if (!draft) {
            return std::unexpected(draft.error());
        }
        (**draft)["count"] = 9;
        return {};
    });
    auto retired = commit([&](Transaction& tx) { return tx.retireDoc(counter(), on(conversationId)); });
    ASSERT_EQ(retired.size(), 1u);
    EXPECT_TRUE(retired[0].at("value").is_null());
    EXPECT_EQ(retired[0].at("record").value("retiredAt", 0) > 0, true);
    auto fresh = commit([&](Transaction& tx) -> Result<void> {
        auto draft = tx.doc(counter(), on(conversationId));
        if (!draft) {
            return std::unexpected(draft.error());
        }
        EXPECT_EQ((**draft).at("count"), 0);
        return {};
    });
    EXPECT_EQ(fresh.size(), 1u);
}

TEST_F(TransactionTest, ForkCopiesCurrentPolicyDocuments) {
    std::int64_t conversationId = makeConversation();
    Json entry;
    commit([&](Transaction& tx) -> Result<void> {
        auto draft = tx.doc(counter(), on(conversationId));
        if (!draft) {
            return std::unexpected(draft.error());
        }
        (**draft)["count"] = 5;
        auto appended = tx.appendEntry(conversationId, Json::object({{"kind", "message"}}));
        if (!appended) {
            return std::unexpected(appended.error());
        }
        entry = *appended;
        return {};
    });
    std::int64_t forkId = 0;
    auto publications = commit([&](Transaction& tx) -> Result<void> {
        auto forked = tx.forkConversation(conversationId, entry.at("id").get<std::int64_t>(), Json::object({{"kind", "ownerless"}}));
        if (!forked) {
            return std::unexpected(forked.error());
        }
        forkId = forked->at("id").get<std::int64_t>();
        auto draft = tx.doc(counter(), on(forkId));
        if (!draft) {
            return std::unexpected(draft.error());
        }
        EXPECT_EQ((**draft).at("count"), 5);
        (**draft)["count"] = 6;
        return {};
    });
    ASSERT_EQ(publications.size(), 1u);
    EXPECT_EQ(publications[0].at("value").at("count"), 6);
    auto parent = m_host.load(counter(), R"(["counter","conversation",)" + std::to_string(conversationId) + R"(,null])",
                              DocumentAddress{"counter", Json::object({{"kind", "conversation"}, {"conversationId", conversationId}}), std::nullopt});
    ASSERT_TRUE(parent.has_value());
    EXPECT_EQ((*parent)->value.at("count"), 5);
}

TEST_F(TransactionTest, ForkCannotChangeCurrentPolicyDocumentsOfItsParent) {
    std::int64_t conversationId = makeConversation();
    Json entry;
    commit([&](Transaction& tx) -> Result<void> {
        auto appended = tx.appendEntry(conversationId, Json::object({{"kind", "message"}}));
        if (!appended) {
            return std::unexpected(appended.error());
        }
        entry = *appended;
        return {};
    });
    auto result = failing([&](Transaction& tx) -> Result<void> {
        auto forked = tx.forkConversation(conversationId, entry.at("id").get<std::int64_t>(), Json::object({{"kind", "ownerless"}}));
        if (!forked) {
            return std::unexpected(forked.error());
        }
        return tx.doc(counter(), on(conversationId)).transform([](Json*) {});
    });
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("while changing its current-policy documents"), std::string::npos);
}

TEST_F(TransactionTest, TaskCreationAndTerminalSettlementRetireTaskDocuments) {
    std::int64_t conversationId = makeConversation();
    std::int64_t taskId = 0;
    DocDefinition notes = counter();
    notes.kind = "notes";
    notes.scope = "task";
    notes.history.clear();
    notes.fork.clear();
    TaskOptions options;
    options.ownership = Json::object({{"kind", "conversation"}});
    options.conversationId = conversationId;
    commit([&](Transaction& tx) -> Result<void> {
        auto created = tx.createTask("work", 1, Json::object({{"x", 1}}), Json::object({{"phase", "ready"}}), options);
        if (!created) {
            return std::unexpected(created.error());
        }
        taskId = *created;
        return tx.doc(notes, on(taskId)).transform([](Json*) {});
    });
    auto stored = m_storage.task(taskId);
    ASSERT_TRUE(stored.has_value() && stored->has_value());
    Json terminal = **stored;
    terminal["state"] = Json::object({{"status", "terminal"}, {"outcome", Json::object({{"status", "done"}})}});
    auto publications = commit([&](Transaction& tx) { return tx.setTask(terminal); });
    ASSERT_EQ(publications.size(), 1u);
    EXPECT_TRUE(publications[0].at("value").is_null());
    EXPECT_EQ(publications[0].at("conversationId"), conversationId);
    auto docs = m_storage.findDocument(DocumentAddress{"notes", Json::object({{"kind", "task"}, {"taskId", taskId}}), std::nullopt}, DocumentPoint{true, 0});
    ASSERT_TRUE(docs.has_value());
    EXPECT_FALSE(docs->has_value());
}

TEST_F(TransactionTest, ChildTaskNeedsALiveOwner) {
    std::int64_t conversationId = makeConversation();
    TaskOptions root;
    root.ownership = Json::object({{"kind", "conversation"}});
    root.conversationId = conversationId;
    std::int64_t parentId = 0;
    commit([&](Transaction& tx) -> Result<void> {
        auto created = tx.createTask("parent", 1, Json::object(), Json::object(), root);
        if (!created) {
            return std::unexpected(created.error());
        }
        parentId = *created;
        return {};
    });
    TaskOptions child;
    child.ownership = Json::object({{"kind", "task"}, {"taskId", parentId}});
    child.background = true;
    auto bad = failing([&](Transaction& tx) { return tx.createTask("child", 1, Json::object(), Json::object(), child).transform([](std::int64_t) {}); });
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().message.find("cannot be background"), std::string::npos);
    child.background = false;
    EXPECT_TRUE(failing([&](Transaction& tx) { return tx.createTask("child", 1, Json::object(), Json::object(), child).transform([](std::int64_t) {}); }).has_value());
}

TEST_F(TransactionTest, SubmissionPlacementAndSettlement) {
    std::int64_t conversationId = makeConversation();
    std::int64_t submissionId = 0;
    Json draft = Json::object({{"conversationId", conversationId},
                               {"type", "input"},
                               {"requestId", "r1"},
                               {"status", "queued"},
                               {"data", Json::object()}});
    commit([&](Transaction& tx) -> Result<void> {
        auto created = tx.createSubmission(draft);
        if (!created) {
            return std::unexpected(created.error());
        }
        submissionId = created->at("id").get<std::int64_t>();
        return {};
    });
    Json entry;
    commit([&](Transaction& tx) -> Result<void> {
        auto appended = tx.appendEntry(conversationId, Json::object({{"kind", "message"}}));
        if (!appended) {
            return std::unexpected(appended.error());
        }
        entry = *appended;
        return tx.placeSubmission(submissionId, entry.at("id").get<std::int64_t>());
    });
    auto placed = m_storage.submission(submissionId);
    ASSERT_TRUE(placed.has_value() && placed->has_value());
    EXPECT_EQ((*placed)->at("status"), "placed");
    commit([&](Transaction& tx) {
        return tx.settleSubmission(submissionId, Json::object({{"status", "done"}, {"answer", entry.at("id")}}));
    });
    auto done = m_storage.submission(submissionId);
    ASSERT_TRUE(done.has_value() && done->has_value());
    EXPECT_EQ((*done)->at("status"), "done");
    // Settling an already settled submission changes nothing.
    EXPECT_TRUE(commit([&](Transaction& tx) {
        return tx.settleSubmission(submissionId, Json::object({{"status", "unanswered"}, {"reason", "late"}}));
    }).empty());
}

TEST_F(TransactionTest, DocumentVersionAndSemanticsAreChecked) {
    std::int64_t conversationId = makeConversation();
    commit([&](Transaction& tx) { return tx.doc(counter(2), on(conversationId)).transform([](Json*) {}); });
    auto older = failing([&](Transaction& tx) { return tx.doc(counter(1), on(conversationId)).transform([](Json*) {}); });
    ASSERT_FALSE(older.has_value());
    EXPECT_NE(older.error().message.find("has newer version 2 than 1"), std::string::npos);
    DocDefinition mismatch = counter(2);
    mismatch.fork = "initial";
    auto semantics = failing([&](Transaction& tx) { return tx.doc(mismatch, on(conversationId)).transform([](Json*) {}); });
    ASSERT_FALSE(semantics.has_value());
    EXPECT_NE(semantics.error().message.find("does not match the supplied definition semantics"), std::string::npos);
}

TEST_F(TransactionTest, SealedTransactionRefusesWork) {
    Transaction tx(m_host, {}, nullptr);
    tx.settleFailure();
    EXPECT_FALSE(tx.createConversation(Json::object({{"kind", "ownerless"}})).has_value());
    EXPECT_FALSE(tx.doc(counter(), on(1)).has_value());
}
