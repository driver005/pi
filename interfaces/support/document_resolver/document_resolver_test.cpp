#include <gtest/gtest.h>

import std;
import pi.support.document_resolver;

class DocumentResolverTest : public ::testing::Test {
protected:
    DocDefinition definition(const std::string& scope, bool family) {
        DocDefinition def;
        def.kind = "notes";
        def.version = 2;
        def.scope = scope;
        def.family = family;
        if (scope == "conversation") {
            def.history = "latest";
            def.fork = "current";
        }
        return def;
    }

    DocumentResolver m_resolver;
};

TEST_F(DocumentResolverTest, SessionSingletonIdentity) {
    auto resolved = m_resolver.resolve(definition("session", false), DocAddressArgs{});
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->id, R"(["notes","session",null,null])");
}

TEST_F(DocumentResolverTest, ConversationFamilyIdentity) {
    DocAddressArgs args;
    args.owner = 7;
    args.key = "a";
    auto resolved = m_resolver.resolve(definition("conversation", true), args);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->id, R"(["notes","conversation",7,"a"])");
    EXPECT_EQ(m_resolver.recordAddressId(Json::object({{"kind", "notes"},
                                                       {"key", "a"},
                                                       {"scope", Json::object({{"kind", "conversation"}, {"conversationId", 7}})}})),
              resolved->id);
}

TEST_F(DocumentResolverTest, MissingOwnerOrKeyFails) {
    EXPECT_FALSE(m_resolver.resolve(definition("task", false), DocAddressArgs{}).has_value());
    DocAddressArgs args;
    args.owner = 1;
    EXPECT_FALSE(m_resolver.resolve(definition("conversation", true), args).has_value());
}

TEST_F(DocumentResolverTest, CreateRecordCarriesConversationSemantics) {
    DocAddressArgs args;
    args.owner = 3;
    auto resolved = m_resolver.resolve(definition("conversation", false), args);
    ASSERT_TRUE(resolved.has_value());
    Json record = m_resolver.createRecord(definition("conversation", false), resolved->address, 11);
    EXPECT_EQ(record.at("history"), "latest");
    EXPECT_EQ(record.at("fork"), "current");
    EXPECT_EQ(record.at("id"), 11);
    EXPECT_TRUE(m_resolver.checkScope(definition("conversation", false), record).has_value());
    EXPECT_FALSE(m_resolver.checkScope(definition("session", false), record).has_value());
}

TEST_F(DocumentResolverTest, VersionChecksAndMigration) {
    Json record = Json::object({{"id", 5}, {"kind", "notes"}, {"scope", Json::object({{"kind", "session"}})}});
    DocDefinition def = definition("session", false);
    EXPECT_FALSE(m_resolver.checkVersion(def, record, 3).has_value());
    EXPECT_FALSE(m_resolver.checkVersion(def, record, 1).has_value());
    def.migrate = [](const Json& value, std::int64_t) {
        Json out = value;
        out["migrated"] = true;
        return out;
    };
    auto migrated = m_resolver.materialize(def, record, 1, Json::object());
    ASSERT_TRUE(migrated.has_value());
    EXPECT_EQ(migrated->at("migrated"), true);
    auto same = m_resolver.materialize(def, record, 2, Json::object({{"a", 1}}));
    ASSERT_TRUE(same.has_value());
    EXPECT_EQ(same->at("a"), 1);
}
