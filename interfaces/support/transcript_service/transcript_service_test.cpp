#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.transcript_service;
import pi.session.session_manager;
import pi.testing.session_harness;
import pi.testing.test_session_handle;

class TranscriptServiceTest : public ::testing::Test {
protected:
    TranscriptServiceTest() {
        SessionManagerOptions options;
        options.cwd = "/work";
        options.persist = false;
        auto manager = std::make_unique<SessionManager>(options, m_harness.files(), m_harness.clock(), m_harness.ids());
        manager->open();
        SessionRuntimeRequest request;
        request.cwd = "/work";
        request.agentDir = "/agent";
        request.sessionManager = std::move(manager);
        m_handle = std::make_unique<TestSessionHandle>(std::move(request), m_harness.agents(), m_harness.provider(),
                                                       m_harness.files(), m_harness.clock(), m_harness.ids(),
                                                       m_harness.sleeper());
        m_service = std::make_unique<TranscriptService>(m_handle->session());
        m_state = m_service->states().at("state");
    }

    SessionHarness m_harness{"/work"};
    std::unique_ptr<TestSessionHandle> m_handle;
    std::unique_ptr<TranscriptService> m_service;
    IReplicatedState* m_state = nullptr;
};

TEST_F(TranscriptServiceTest, StartsEmptyAndHasNoMethods) {
    const Json value = m_state->snapshot().value;
    EXPECT_TRUE(value["messages"].empty());
    EXPECT_EQ(value["isStreaming"], false);
    EXPECT_TRUE(m_service->methods().empty());
}

TEST_F(TranscriptServiceTest, TheConversationFollowsTheSession) {
    std::vector<std::int64_t> sequences;
    m_state->subscribe([&](const Json&, std::int64_t sequence, const ServiceContext&) { sequences.push_back(sequence); });
    m_harness.provider().enqueue(m_harness.provider().textResponse("hello there"));
    ASSERT_TRUE(m_handle->session().prompt("hi", {}));
    const Json value = m_state->snapshot().value;
    ASSERT_EQ(value["messages"].size(), 3u);
    EXPECT_EQ(value["messages"][0]["role"], "system");
    EXPECT_EQ(value["messages"][1]["role"], "user");
    EXPECT_EQ(value["messages"][2]["role"], "assistant");
    EXPECT_EQ(value["messages"][2]["content"][0]["text"], "hello there");
    EXPECT_EQ(value["isStreaming"], false);
    ASSERT_FALSE(sequences.empty());
    for (std::size_t i = 1; i < sequences.size(); ++i) {
        EXPECT_EQ(sequences[i], sequences[i - 1] + 1);
    }
    EXPECT_EQ(sequences.back(), m_state->snapshot().sequence);
}

TEST_F(TranscriptServiceTest, TheStateCoversStreamingProgress) {
    bool sawStreaming = false;
    m_state->subscribe([&](const Json&, std::int64_t, const ServiceContext&) {
        if (m_state->snapshot().value["isStreaming"] == true) {
            sawStreaming = true;
        }
    });
    m_harness.provider().enqueue(m_harness.provider().textResponse("a fairly long answer that streams in chunks"));
    ASSERT_TRUE(m_handle->session().prompt("hi", {}));
    EXPECT_TRUE(sawStreaming);
}
