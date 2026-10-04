#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.thread_pool;
import pi.serve.served_session_opener;
import pi.session.session_store;
import pi.session_manager_factory;
import pi.support.served_session;
import pi.testing.fake_model_runtime;
import pi.testing.sequential_id_generator;
import pi.testing.session_harness;
import pi.testing.session_runtime_factory;

class ServedSessionOpenerTest : public ::testing::Test {
protected:
    ServedSessionOpenerTest() {
        m_harness.files().createDirectories("/srv/sessions/alpha");
        m_record = SessionRecord{"alpha", 1000, "/work", "/srv/sessions/alpha"};
    }

    ServiceContext context() const {
        return ServiceContext{std::make_shared<AbortSignal>()};
    }

    SessionHarness m_harness{"/work"};
    SessionManagerFactory m_managers{m_harness.files(), m_harness.clock(), m_harness.ids()};
    SessionStore m_store{"/agent", m_harness.files(), m_harness.clock(), m_harness.ids(), m_managers};
    SessionRuntimeFactory m_runtimes{m_harness.agents(), m_harness.provider(), m_harness.files(), m_harness.clock(),
                                     m_harness.ids(),    m_harness.sleeper()};
    FakeModelRuntime m_models;
    ThreadPool m_executor{2};
    SequentialIdGenerator m_ids{"op"};
    ServedSessionOpener m_opener{m_store, m_runtimes, m_models, m_executor, m_ids, "/agent"};
    SessionRecord m_record;
};

TEST_F(ServedSessionOpenerTest, OpensASessionNamedAfterTheCatalogEntry) {
    auto handle = m_opener.open(m_record, context());
    ASSERT_TRUE(handle);
    auto* served = dynamic_cast<ServedSession*>(handle->get());
    ASSERT_TRUE(served != nullptr);
    EXPECT_EQ(served->runtime().session().sessionId(), "alpha");
    EXPECT_EQ(served->runtime().cwd(), "/work");
    EXPECT_EQ(served->runtime().agentDir(), "/agent");
    EXPECT_EQ(m_runtimes.reasons().size(), 1u);
}

TEST_F(ServedSessionOpenerTest, RuntimeFailuresAreReported) {
    m_runtimes.failNext("no runtime today");
    const auto handle = m_opener.open(m_record, context());
    ASSERT_FALSE(handle);
    EXPECT_EQ(handle.error().message, "no runtime today");
}

TEST_F(ServedSessionOpenerTest, ReopeningContinuesTheSessionFile) {
    {
        auto handle = m_opener.open(m_record, context());
        ASSERT_TRUE(handle);
        auto* served = dynamic_cast<ServedSession*>(handle->get());
        m_harness.provider().enqueue(m_harness.provider().textResponse("first"));
        ASSERT_TRUE(served->runtime().session().prompt("hello", {}));
        (*handle)->close(context());
    }
    auto again = m_opener.open(m_record, context());
    ASSERT_TRUE(again);
    auto* served = dynamic_cast<ServedSession*>(again->get());
    EXPECT_EQ(served->runtime().session().sessionId(), "alpha");
    EXPECT_FALSE(served->runtime().session().messages().empty());
}
