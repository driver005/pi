#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.conversation_views;
import pi.support.delta_applier;
import pi.support.json_equality;

class ConversationViewsTest : public ::testing::Test {
protected:
    ConversationViewsTest()
        : m_session(m_storage, [this](Transaction& tx, const Json& record) -> Result<void> {
              DocAddressArgs args;
              args.owner = record.at("id").get<std::int64_t>();
              for (const DocDefinition& definition : {m_documents.live(), m_documents.inbox(), m_documents.usage(), m_documents.agent()}) {
                  if (auto doc = tx.doc(definition, args); !doc) {
                      return std::unexpected(doc.error());
                  }
              }
              return {};
          }),
          m_views(m_session) {}

    std::int64_t newConversation() {
        std::int64_t id = 0;
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
            if (!created) {
                return std::unexpected(created.error());
            }
            id = created->at("id").get<std::int64_t>();
            return {};
        }).has_value());
        return id;
    }

    std::int64_t append(std::int64_t conversationId, const Json& extra = Json::object()) {
        std::int64_t id = 0;
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            Json draft = Json::object({{"kind", "m"}});
            for (const auto& item : extra.items()) {
                draft[item.key()] = item.value();
            }
            auto entry = tx.appendEntry(conversationId, draft);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            id = entry->at("id").get<std::int64_t>();
            return {};
        }).has_value());
        return id;
    }

    Result<void> editInbox(std::int64_t conversationId, const Json& item) {
        DocAddressArgs args;
        args.owner = conversationId;
        auto committed = m_session.commit([&](Transaction& tx) -> Result<void> {
            auto doc = tx.doc(m_documents.inbox(), args);
            if (!doc) {
                return std::unexpected(doc.error());
            }
            (**doc)["items"].push_back(item);
            return {};
        });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        return {};
    }

    std::shared_ptr<MemoryStorage> m_storage = std::make_shared<MemoryStorage>();
    BuiltinDocuments m_documents;
    DurableSession m_session;
    ConversationViews m_views;
};

TEST_F(ConversationViewsTest, MountsTheConversationItsEntriesAndItsBuiltInDocuments) {
    const std::int64_t id = newConversation();
    const std::int64_t first = append(id);
    auto view = m_views.state(id);
    ASSERT_TRUE(view.has_value()) << view.error().message;
    const Json value = (*view)->snapshot().value;
    EXPECT_EQ(value.at("conversation").at("id"), id);
    ASSERT_EQ(value.at("entries").size(), 1u);
    EXPECT_EQ(value.at("entries")[0].at("id"), first);
    for (const char* kind : {"pi.agent", "pi.live", "pi.inbox", "pi.usage"}) {
        EXPECT_TRUE(value.at("docs").contains(kind)) << kind;
    }
}

TEST_F(ConversationViewsTest, UnknownConversationsCannotBeMounted) {
    auto view = m_views.state(77);
    ASSERT_FALSE(view.has_value());
    EXPECT_NE(view.error().message.find("does not exist"), std::string::npos);
}

TEST_F(ConversationViewsTest, FollowsEntriesDocumentChangesAndOtherConversationsAreIgnored) {
    const std::int64_t id = newConversation();
    const std::int64_t other = newConversation();
    auto view = m_views.state(id);
    ASSERT_TRUE(view.has_value());
    std::vector<Json> published;
    (*view)->subscribe([&](const Json& ops, std::int64_t, const ServiceContext&) { published.push_back(ops); });
    append(other);
    EXPECT_TRUE(published.empty());
    const std::int64_t entry = append(id);
    ASSERT_EQ(published.size(), 1u);
    EXPECT_EQ((*view)->snapshot().value.at("entries")[0].at("id"), entry);
    ASSERT_TRUE(editInbox(id, Json::object({{"id", 5}})).has_value());
    EXPECT_EQ((*view)->snapshot().value.at("docs").at("pi.inbox").at("items").size(), 1u);
    EXPECT_EQ(published.size(), 2u);
    EXPECT_EQ((*view)->snapshot().sequence, 2);
    ASSERT_TRUE(editInbox(other, Json::object({{"id", 6}})).has_value());
    EXPECT_EQ(published.size(), 2u);
}

TEST_F(ConversationViewsTest, ReplayingThePublishedOpsRebuildsTheValueAndHeadMarkersReplaceTheirPrefix) {
    const std::int64_t id = newConversation();
    auto view = m_views.state(id);
    ASSERT_TRUE(view.has_value());
    Json replay = (*view)->snapshot().value;
    (*view)->subscribe([&](const Json& ops, std::int64_t, const ServiceContext&) {
        auto applied = DeltaApplier().apply(replay, ops);
        ASSERT_TRUE(applied.has_value());
        replay = *applied;
    });
    append(id);
    const std::int64_t kept = append(id);
    append(id);
    const std::int64_t marker = append(id, Json::object({{"head", kept}}));
    const Json value = (*view)->snapshot().value;
    // The marker, then the non-head entries from its head.
    ASSERT_EQ(value.at("entries").size(), 3u);
    EXPECT_EQ(value.at("entries")[0].at("id"), marker);
    EXPECT_EQ(value.at("entries")[1].at("id"), kept);
    EXPECT_TRUE(JsonEquality().equal(replay, value));
    // A mount built from storage agrees with the one maintained from publications.
    auto fresh = ConversationViews(m_session).state(id);
    ASSERT_TRUE(fresh.has_value());
    EXPECT_TRUE(JsonEquality().equal((*fresh)->snapshot().value, value));
}

TEST_F(ConversationViewsTest, SharesOneMountWhileHeldAndRebuildsAfterRelease) {
    const std::int64_t id = newConversation();
    auto first = m_views.state(id);
    auto second = m_views.state(id);
    ASSERT_TRUE(first.has_value() && second.has_value());
    EXPECT_EQ(first->get(), second->get());
    const IReplicatedState* released = first->get();
    first->reset();
    second->reset();
    append(id);
    auto rebuilt = m_views.state(id);
    ASSERT_TRUE(rebuilt.has_value());
    EXPECT_EQ((*rebuilt)->snapshot().value.at("entries").size(), 1u);
    EXPECT_EQ((*rebuilt)->snapshot().sequence, 0);
    (void)released;
}

TEST_F(ConversationViewsTest, ClosedSessionsRefuseMounts) {
    const std::int64_t id = newConversation();
    ASSERT_TRUE(m_session.close().has_value());
    EXPECT_FALSE(m_views.state(id).has_value());
}

TEST_F(ConversationViewsTest, ObserversSeeEveryPublicationAfterTheirStartingViewUntilTheyLeave) {
    const std::int64_t id = newConversation();
    const std::int64_t other = newConversation();
    auto held = m_views.state(id);
    ASSERT_TRUE(held.has_value());
    append(id);
    std::vector<Json> seen;
    bool hydrated = false;
    auto observation = m_views.observe(
        id,
        [&](IStorage& storage, const Json& value) -> Result<void> {
            hydrated = value.at("entries").size() == 1u && storage.conversation(id).has_value();
            return {};
        },
        [&](const Json& before, const Json& after, const Json& ops, const Json& publication) { seen.push_back(Json{{"before", before}, {"after", after}, {"ops", ops}, {"seq", publication.at("seq")}}); }, [] {});
    ASSERT_TRUE(observation.has_value()) << observation.error().message;
    EXPECT_TRUE(hydrated);
    EXPECT_EQ(observation->value.at("entries").size(), 1u);
    append(id);
    append(other);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].at("before").at("entries").size(), 1u);
    EXPECT_EQ(seen[0].at("after").at("entries").size(), 2u);
    EXPECT_FALSE(seen[0].at("ops").empty());
    // A publication that changes nothing in this view still reaches the observer, without operations.
    EXPECT_EQ(seen[1].at("before"), seen[1].at("after"));
    EXPECT_TRUE(seen[1].at("ops").empty());
    EXPECT_GT(seen[1].at("seq").get<std::int64_t>(), seen[0].at("seq").get<std::int64_t>());
    m_views.unobserve(id, observation->id);
    append(id);
    EXPECT_EQ(seen.size(), 2u);
}

TEST_F(ConversationViewsTest, ObservingNeedsAHeldViewAndClosingTheSessionNotifiesObservers) {
    const std::int64_t id = newConversation();
    auto none = m_views.observe(id, nullptr, [](const Json&, const Json&, const Json&, const Json&) {}, [] {});
    ASSERT_FALSE(none.has_value());
    auto held = m_views.state(id);
    ASSERT_TRUE(held.has_value());
    bool closed = false;
    ASSERT_TRUE(m_views.observe(id, nullptr, [](const Json&, const Json&, const Json&, const Json&) {}, [&] { closed = true; }).has_value());
    ASSERT_TRUE(m_session.close().has_value());
    EXPECT_TRUE(closed);
}
