#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.submissions;

class SubmissionsTest : public ::testing::Test {
protected:
    SubmissionsTest()
        : m_session(m_storage), m_submissions(m_session, [] { return std::int64_t(77); }, [] { return ResolvedSettings{}; }, [this] { ++m_resumes; }) {
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
            if (!created) {
                return std::unexpected(created.error());
            }
            m_conversation = created->at("id").get<std::int64_t>();
            return {};
        }).has_value());
    }

    SubmissionDraft input(const std::string& text) {
        SubmissionDraft draft;
        draft.type = "input";
        draft.content = text;
        return draft;
    }

    std::shared_ptr<MemoryStorage> m_storage = std::make_shared<MemoryStorage>();
    DurableSession m_session;
    Submissions m_submissions;
    std::atomic<int> m_resumes{0};
    std::int64_t m_conversation = 0;
};

TEST_F(SubmissionsTest, SubmitAdmitsResumesAndReturnsAHandle) {
    auto handle = m_submissions.submit(m_conversation, input("hi"));
    ASSERT_TRUE(handle.has_value());
    EXPECT_EQ(m_resumes.load(), 1);
    auto status = (*handle)->status();
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->at("status"), "placed");
    auto again = m_submissions.get((*handle)->id());
    ASSERT_TRUE(again.has_value() && *again);
    EXPECT_EQ((*again)->id(), (*handle)->id());
    auto missing = m_submissions.get(99999);
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(*missing);
    EXPECT_FALSE(m_submissions.status(99999).has_value());
}

TEST_F(SubmissionsTest, WaitReturnsSettledRecordsAtOnceAndBlocksUntilSettlement) {
    auto handle = m_submissions.submit(m_conversation, input("hi"));
    ASSERT_TRUE(handle.has_value());
    std::atomic<bool> done{false};
    Json settled;
    std::thread waiter([&] {
        auto result = (*handle)->wait();
        if (result) {
            settled = *result;
        }
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_FALSE(done);
    // Settle it as a generation would.
    const std::int64_t id = (*handle)->id();
    ASSERT_TRUE(m_session.commit([&](Transaction& tx) { return tx.settleSubmission(id, Json::object({{"status", "done"}, {"answer", 5}})); }).has_value());
    waiter.join();
    EXPECT_TRUE(done);
    EXPECT_EQ(settled.at("status"), "done");
    auto instant = (*handle)->wait();
    ASSERT_TRUE(instant.has_value());
    EXPECT_EQ(instant->at("answer"), 5);
}

TEST_F(SubmissionsTest, WaitFailsOnCancellationUnknownIdsAndClose) {
    auto handle = m_submissions.submit(m_conversation, input("hi"));
    ASSERT_TRUE(handle.has_value());
    AbortSignal cancel;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        cancel.abort();
    });
    auto cancelled = (*handle)->wait(&cancel);
    canceller.join();
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error().code, "aborted");
    EXPECT_FALSE(m_submissions.wait(424242, nullptr).has_value());
    std::atomic<bool> rejected{false};
    std::thread waiter([&] {
        auto result = (*handle)->wait();
        rejected = !result.has_value() && result.error().code == "harness_closed";
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ASSERT_TRUE(m_session.close().has_value());
    waiter.join();
    EXPECT_TRUE(rejected);
}

TEST_F(SubmissionsTest, AbortWithdrawsQueuedWorkAndReportsPlacedAndSettledOnes) {
    auto first = m_submissions.submit(m_conversation, input("runs"));
    auto queued = m_submissions.submit(m_conversation, input("waits"));
    ASSERT_TRUE(first.has_value() && queued.has_value());
    EXPECT_EQ(*(*first)->abort(), "already_placed");
    EXPECT_EQ(*(*queued)->abort(), "aborted");
    auto record = (*queued)->status();
    EXPECT_EQ(record->at("status"), "unanswered");
    EXPECT_EQ(record->at("reason"), "aborted");
    EXPECT_EQ(*(*queued)->abort(), "settled");
    EXPECT_EQ(*m_submissions.abort((*queued)->id(), m_conversation + 100), "not_found");
    EXPECT_EQ(*m_submissions.abort(31337, std::nullopt), "not_found");
    EXPECT_EQ(*m_submissions.abort((*first)->id(), m_conversation), "already_placed");
}
