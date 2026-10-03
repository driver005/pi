#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.durable_session;
import pi.support.usage_ledger;

class UsageLedgerTest : public ::testing::Test {
protected:
    Json usage(int input, int output, double cost) {
        return Json::object({{"input", input}, {"output", output}, {"cacheRead", 0}, {"cacheWrite", 0}, {"totalTokens", input + output},
                             {"cost", Json::object({{"input", cost}, {"output", 0.0}, {"cacheRead", 0.0}, {"cacheWrite", 0.0}, {"total", cost}})}});
    }

    UsageLedger m_ledger;
};

TEST_F(UsageLedgerTest, AddSumsCountersAndKeepsIntegersIntegral) {
    Json total = usage(10, 5, 0.25);
    Json extra = usage(1, 2, 0.5);
    extra["reasoning"] = 3;
    m_ledger.add(total, extra);
    EXPECT_EQ(total.at("input"), 11);
    EXPECT_TRUE(total.at("input").is_number_integer());
    EXPECT_EQ(total.at("totalTokens"), 18);
    EXPECT_EQ(total.at("reasoning"), 3);
    EXPECT_DOUBLE_EQ(total.at("cost").at("input").get<double>(), 0.75);
    m_ledger.add(total, extra);
    EXPECT_EQ(total.at("reasoning"), 6);
}

TEST_F(UsageLedgerTest, AddStateMergesBuckets) {
    Json sum = m_ledger.empty();
    Json first = m_ledger.empty();
    first["models"]["a/m"] = usage(1, 1, 0.1);
    Json second = m_ledger.empty();
    second["models"]["a/m"] = usage(2, 2, 0.1);
    second["tools"]["bash"] = usage(0, 0, 0.0);
    m_ledger.addState(sum, first);
    m_ledger.addState(sum, second);
    EXPECT_EQ(sum.at("models").at("a/m").at("input"), 3);
    EXPECT_TRUE(sum.at("tools").contains("bash"));
}

TEST_F(UsageLedgerTest, RecordWritesTheConversationsLedgerInTheCommit) {
    auto storage = std::make_shared<MemoryStorage>();
    DurableSession session(storage);
    std::int64_t conversationId = 0;
    ASSERT_TRUE(session.commit([&](Transaction& tx) -> Result<void> {
        auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
        if (!created) {
            return std::unexpected(created.error());
        }
        conversationId = created->at("id").get<std::int64_t>();
        return {};
    }).has_value());
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(session.commit([&](Transaction& tx) { return m_ledger.record(tx, conversationId, "models", "p/m", usage(3, 4, 1.0)); }).has_value());
    }
    BuiltinDocuments documents;
    DocAddressArgs args;
    args.owner = conversationId;
    auto snapshot = session.snapshot(documents.usage(), args);
    ASSERT_TRUE(snapshot.has_value() && snapshot->has_value());
    EXPECT_EQ((*snapshot)->at("models").at("p/m").at("input"), 6);
    EXPECT_EQ((*snapshot)->at("models").at("p/m").at("totalTokens"), 14);
}
