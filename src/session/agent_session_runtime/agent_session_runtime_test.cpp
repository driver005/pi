#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.session.agent_session_runtime;
import pi.session.session_store;
import pi.session_manager_factory;
import pi.testing.session_harness;
import pi.testing.session_runtime_factory;

class AgentSessionRuntimeTest : public testing::Test {
protected:
    AgentSessionRuntimeTest() {
        m_harness.files().createDirectories("/work");
        m_harness.files().createDirectories("/other");
        m_runtime = std::make_unique<AgentSessionRuntime>(m_factory, m_store, m_harness.files(),
                                                          handleFor("/work", "startup"));
    }

    std::unique_ptr<ISessionRuntimeHandle> handleFor(const std::string& cwd, const std::string& reason) {
        SessionRuntimeRequest request;
        request.cwd = cwd;
        request.agentDir = "/agent";
        request.startReason = reason;
        auto manager = m_store.create(cwd, std::nullopt, std::nullopt, std::nullopt);
        EXPECT_TRUE(manager.has_value());
        request.sessionManager = std::move(*manager);
        auto handle = m_factory.create(std::move(request));
        EXPECT_TRUE(handle.has_value());
        return std::move(*handle);
    }

    void say(const std::string& text, const std::string& reply) {
        m_harness.provider().enqueue(m_harness.provider().textResponse(reply));
        ASSERT_TRUE(m_runtime->session().prompt(text, {}).has_value());
    }

    SessionHarness m_harness;
    SessionManagerFactory m_managers{m_harness.files(), m_harness.clock(), m_harness.ids()};
    SessionStore m_store{"/agent", m_harness.files(), m_harness.clock(), m_harness.ids(), m_managers};
    SessionRuntimeFactory m_factory{m_harness.agents(), m_harness.provider(), m_harness.files(), m_harness.clock(),
                                    m_harness.ids(), m_harness.sleeper()};
    std::unique_ptr<AgentSessionRuntime> m_runtime;
};

TEST_F(AgentSessionRuntimeTest, NewSessionReplacesTheCurrentOne) {
    say("first", "one");
    const std::string oldId = m_runtime->session().sessionId();
    const auto oldFile = m_runtime->session().sessionFile();
    ASSERT_TRUE(m_runtime->newSession(std::nullopt).has_value());
    EXPECT_NE(m_runtime->session().sessionId(), oldId);
    EXPECT_TRUE(m_runtime->session().messages().empty());
    EXPECT_EQ(m_factory.reasons(), (std::vector<std::string>{"startup", "new"}));
    ASSERT_TRUE(oldFile.has_value());
    EXPECT_TRUE(m_harness.files().exists(*oldFile));
}

TEST_F(AgentSessionRuntimeTest, SwitchSessionResumesAnotherFile) {
    say("first", "one");
    const std::string firstId = m_runtime->session().sessionId();
    const std::string firstFile = *m_runtime->session().sessionFile();
    ASSERT_TRUE(m_runtime->newSession(std::nullopt).has_value());
    say("second", "two");
    ASSERT_TRUE(m_runtime->switchSession(firstFile, std::nullopt).has_value());
    EXPECT_EQ(m_runtime->session().sessionId(), firstId);
    EXPECT_EQ(m_runtime->session().lastAssistantText(), "one");
    EXPECT_EQ(m_factory.reasons().back(), "resume");
}

TEST_F(AgentSessionRuntimeTest, SwitchToSessionWithMissingCwdIsRefusedUnlessOverridden) {
    auto ghost = m_store.create("/ghost", std::nullopt, std::nullopt, std::nullopt);
    ASSERT_TRUE(ghost.has_value());
    UserMessage hello;
    hello.content = std::string("hello");
    ASSERT_TRUE((*ghost)->appendMessage(hello).has_value());
    const std::string file = *(*ghost)->sessionFile();
    const std::string current = m_runtime->session().sessionId();
    const auto refused = m_runtime->switchSession(file, std::nullopt);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, "missing_session_cwd");
    EXPECT_NE(refused.error().message.find("Stored session working directory does not exist: /ghost"), std::string::npos);
    EXPECT_EQ(m_runtime->session().sessionId(), current);
    ASSERT_TRUE(m_runtime->switchSession(file, std::string("/other")).has_value());
    EXPECT_EQ(m_runtime->cwd(), "/other");
}

TEST_F(AgentSessionRuntimeTest, ForkBeforeAUserMessageContinuesFromItsParent) {
    say("first question", "first answer");
    say("second question", "second answer");
    const auto messages = m_runtime->session().forkableMessages();
    ASSERT_EQ(messages.size(), 2U);
    const std::string originalFile = *m_runtime->session().sessionFile();
    const auto forked = m_runtime->fork(messages[1].entryId, ForkPosition::Before);
    ASSERT_TRUE(forked.has_value()) << forked.error().message;
    EXPECT_EQ(forked->selectedText, "second question");
    EXPECT_NE(*m_runtime->session().sessionFile(), originalFile);
    EXPECT_EQ(m_runtime->session().lastAssistantText(), "first answer");
    EXPECT_EQ(m_runtime->session().forkableMessages().size(), 1U);
    EXPECT_EQ(m_factory.reasons().back(), "fork");
}

TEST_F(AgentSessionRuntimeTest, ForkAtAnEntryKeepsItAndBeforeFirstMessageStartsEmpty) {
    say("first question", "first answer");
    const auto messages = m_runtime->session().forkableMessages();
    ASSERT_TRUE(m_runtime->fork(messages[0].entryId, ForkPosition::At).has_value());
    EXPECT_EQ(m_runtime->session().forkableMessages().size(), 1U);
    const auto fromStart = m_runtime->fork(m_runtime->session().forkableMessages()[0].entryId, ForkPosition::Before);
    ASSERT_TRUE(fromStart.has_value());
    EXPECT_EQ(fromStart->selectedText, "first question");
    // Only the prompt's system message precedes the first user message.
    const auto remaining = m_runtime->session().messages();
    ASSERT_EQ(remaining.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<SystemMessage>(remaining[0]));
}

TEST_F(AgentSessionRuntimeTest, ForkRejectsUnknownEntriesAndNonUserMessages) {
    say("question", "answer");
    EXPECT_FALSE(m_runtime->fork("nope", ForkPosition::Before).has_value());
    // The assistant entry is not a user message, so forking before it is invalid.
    std::string assistantId;
    const auto count = m_runtime->session().stats().totalMessages;
    EXPECT_GE(count, 2);
    const auto result = m_runtime->fork(m_runtime->session().forkableMessages()[0].entryId, ForkPosition::Before);
    EXPECT_TRUE(result.has_value());
}

TEST_F(AgentSessionRuntimeTest, ImportCopiesTheFileIntoTheSessionDirectoryAndSwitches) {
    say("imported question", "imported answer");
    const std::string source = *m_runtime->session().sessionFile();
    const std::string content = *m_harness.files().readFile(source);
    m_harness.files().createDirectories("/tmp/exports");
    ASSERT_TRUE(m_harness.files().writeFile("/tmp/exports/chat.jsonl", content).has_value());
    ASSERT_TRUE(m_runtime->newSession(std::nullopt).has_value());
    ASSERT_TRUE(m_runtime->importFromJsonl("/tmp/exports/chat.jsonl", std::nullopt).has_value());
    EXPECT_EQ(m_runtime->session().lastAssistantText(), "imported answer");
    const std::string stored = *m_runtime->session().sessionFile();
    EXPECT_TRUE(stored.ends_with("chat.jsonl"));
    ASSERT_TRUE(m_runtime->importFromJsonl("/tmp/exports/chat.jsonl", std::nullopt).has_value());
    EXPECT_TRUE(m_runtime->session().sessionFile()->ends_with("chat-1.jsonl"));
    const auto missing = m_runtime->importFromJsonl("/tmp/exports/none.jsonl", std::nullopt);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message, "File not found: /tmp/exports/none.jsonl");
}

TEST_F(AgentSessionRuntimeTest, FactoryFailureIsReported) {
    m_factory.failNext("boom");
    const auto result = m_runtime->newSession(std::nullopt);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "boom");
}

TEST_F(AgentSessionRuntimeTest, DisposeStopsTheSession) {
    m_runtime->dispose();
    EXPECT_TRUE(m_runtime->session().isIdle());
}
