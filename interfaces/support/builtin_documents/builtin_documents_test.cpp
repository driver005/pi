#include <gtest/gtest.h>

import std;
import pi.support.builtin_documents;

class BuiltinDocumentsTest : public ::testing::Test {
protected:
    BuiltinDocuments m_documents;
};

TEST_F(BuiltinDocumentsTest, SemanticsMatchTheSpec) {
    const DocDefinition agent = m_documents.agent();
    EXPECT_EQ(agent.kind, "pi.agent");
    EXPECT_EQ(agent.history, "rewindable");
    EXPECT_EQ(agent.fork, "asOf");
    for (const DocDefinition& definition : {m_documents.live(), m_documents.inbox(), m_documents.usage()}) {
        EXPECT_EQ(definition.scope, "conversation");
        EXPECT_EQ(definition.history, "latest");
        EXPECT_EQ(definition.fork, "initial");
        EXPECT_EQ(definition.version, 1);
    }
}

TEST_F(BuiltinDocumentsTest, InitialValues) {
    EXPECT_EQ(m_documents.agent().initial(Json(nullptr)), Json::object());
    EXPECT_EQ(m_documents.live().initial(Json(nullptr)), Json::object());
    EXPECT_EQ(m_documents.inbox().initial(Json(nullptr)).dump(), R"({"items":[]})");
    EXPECT_EQ(m_documents.usage().initial(Json(nullptr)).dump(), R"({"models":{},"tools":{}})");
}

TEST_F(BuiltinDocumentsTest, LiveCheckpointsOnlyWhenNothingRuns) {
    const DocDefinition live = m_documents.live();
    EXPECT_TRUE(live.checkpointWhen(Json::object(), Json::array(), 0));
    EXPECT_FALSE(live.checkpointWhen(Json::object({{"generation", Json::object({{"attempt", 1}})}}), Json::array(), 0));
    EXPECT_FALSE(live.checkpointWhen(Json::parse(R"({"tools":[{"status":"done"},{"status":"running"}]})"), Json::array(), 0));
    EXPECT_TRUE(live.checkpointWhen(Json::parse(R"({"tools":[{"status":"done"},{"status":"pending"}]})"), Json::array(), 0));
}

TEST_F(BuiltinDocumentsTest, InboxCheckpointsWhenEmpty) {
    const DocDefinition inbox = m_documents.inbox();
    EXPECT_TRUE(inbox.checkpointWhen(Json::parse(R"({"items":[]})"), Json::array(), 0));
    EXPECT_FALSE(inbox.checkpointWhen(Json::parse(R"({"items":[{"id":1}]})"), Json::array(), 0));
}
