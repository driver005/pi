#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.session_router;
import pi.testing.scripted_server_host;
import pi.testing.sequential_id_generator;

class SessionRouterTest : public ::testing::Test {
protected:
    SessionRouterTest() {
        SessionRouterOptions options;
        options.host = &m_host;
        options.ids = &m_ids;
        options.serverId = "server";
        options.isClosing = [this] { return m_closing.load(); };
        options.publishAttachment = [this](std::uint64_t client, const std::optional<SessionAttachment>& attachment,
                                           const ServiceContext&) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_published.push_back(std::to_string(client) + ":" +
                                (attachment ? attachment->sessionId + "/" + attachment->attachmentId : "none"));
        };
        options.reportError = [this](const Error& error) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_errors.push_back(error.code);
        };
        m_router = std::make_unique<SessionRouter>(options);
    }

    ServiceContext context() const {
        return ServiceContext{std::make_shared<AbortSignal>()};
    }

    RpcTarget target(const std::string& session, const std::string& attachment) const {
        RpcTarget result;
        result.serverId = "server";
        result.sessionId = session;
        result.attachmentId = attachment;
        return result;
    }

    Result<std::optional<Json>> call(std::uint64_t client, const std::string& session, const std::string& attachment) {
        const IServiceEndpoint::Publisher publish = [](const std::string&, const Json&, const ServiceContext&) {};
        return m_router->executeServiceCall(Json{{"member", "ping"}}, target(session, attachment), client, publish, context());
    }

    std::vector<std::string> publications() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_published;
    }

    ScriptedServerHost m_host;
    SequentialIdGenerator m_ids{"att"};
    std::atomic<bool> m_closing{false};
    std::mutex m_mutex;
    std::vector<std::string> m_published;
    std::vector<std::string> m_errors;
    std::unique_ptr<SessionRouter> m_router;
};

TEST_F(SessionRouterTest, AttachOpensTheSessionOnceAndPublishesTheAttachment) {
    auto session = m_host.addSession("s1");
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    ASSERT_TRUE(m_router->attachClient(2, "s1", context()));
    EXPECT_EQ(session->opened.load(), 1);
    EXPECT_EQ(session->attached.load(), 2);
    EXPECT_EQ(publications(), (std::vector<std::string>{"1:s1/att1", "2:s1/att2"}));
}

TEST_F(SessionRouterTest, AttachingTheSameSessionAgainIsANoOp) {
    auto session = m_host.addSession("s1");
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    EXPECT_EQ(session->attached.load(), 1);
    EXPECT_EQ(publications().size(), 1u);
}

TEST_F(SessionRouterTest, PrefixesResolveToTheCanonicalId) {
    m_host.addSession("session-abc");
    ASSERT_TRUE(m_router->attachClient(1, "session-a", context()));
    EXPECT_EQ(publications(), (std::vector<std::string>{"1:session-abc/att1"}));
    ASSERT_TRUE(m_router->attachClient(1, "session-abc", context()));
    EXPECT_EQ(publications().size(), 1u);
}

TEST_F(SessionRouterTest, HostErrorsPassThrough) {
    m_host.addSession("ab1");
    m_host.addSession("ab2");
    EXPECT_EQ(m_router->attachClient(1, "zz", context()).error().code, "session_not_found");
    EXPECT_EQ(m_router->attachClient(1, "ab", context()).error().code, "session_ambiguous");
    auto broken = m_host.addSession("broken");
    broken->openFailure = Error{"open_failed", "no"};
    EXPECT_EQ(m_router->attachClient(1, "broken", context()).error().code, "open_failed");
    EXPECT_TRUE(publications().empty());
}

TEST_F(SessionRouterTest, ServiceCallsReachTheAttachedSession) {
    auto session = m_host.addSession("s1");
    session->handler = [](const Json& call, const IServiceEndpoint::Publisher&, const ServiceContext&) {
        return Result<std::optional<Json>>(std::optional<Json>(call["member"]));
    };
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    auto result = call(1, "s1", "att1");
    ASSERT_TRUE(result);
    EXPECT_EQ(**result, Json("ping"));
}

TEST_F(SessionRouterTest, CallsNeedTheMatchingAttachment) {
    m_host.addSession("s1");
    m_host.addSession("s2");
    EXPECT_EQ(call(1, "s1", "att1").error().code, "session_not_attached");
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    EXPECT_EQ(call(1, "s1", "other").error().code, "session_not_attached");
    EXPECT_EQ(call(1, "s2", "att1").error().code, "session_not_attached");
    EXPECT_EQ(call(2, "s1", "att1").error().code, "session_not_attached");
    RpcTarget serverTarget;
    serverTarget.serverId = "server";
    const IServiceEndpoint::Publisher publish = [](const std::string&, const Json&, const ServiceContext&) {};
    EXPECT_EQ(m_router->executeServiceCall(Json::object(), serverTarget, 1, publish, context()).error().code,
              "session_not_attached");
}

TEST_F(SessionRouterTest, SwitchingSessionsReleasesTheOldAttachment) {
    auto first = m_host.addSession("s1");
    auto second = m_host.addSession("s2");
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    ASSERT_TRUE(m_router->attachClient(1, "s2", context()));
    EXPECT_EQ(first->released.load(), 1);
    EXPECT_EQ(second->attached.load(), 1);
    EXPECT_EQ(publications(), (std::vector<std::string>{"1:s1/att1", "1:s2/att2"}));
    EXPECT_EQ(call(1, "s1", "att1").error().code, "session_not_attached");
    EXPECT_TRUE(call(1, "s2", "att2"));
}

TEST_F(SessionRouterTest, DetachReleasesAndPublishesNone) {
    auto session = m_host.addSession("s1");
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    ASSERT_TRUE(m_router->detachClient(1, context()));
    EXPECT_EQ(session->released.load(), 1);
    EXPECT_EQ(publications().back(), "1:none");
    EXPECT_TRUE(m_router->detachClient(1, context()));
    EXPECT_EQ(publications().size(), 2u);
}

TEST_F(SessionRouterTest, DisconnectReleasesWithoutPublishing) {
    auto session = m_host.addSession("s1");
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    m_router->disconnect(1, context());
    EXPECT_EQ(session->released.load(), 1);
    EXPECT_EQ(publications().size(), 1u);
    EXPECT_EQ(session->closed.load(), 0);
}

TEST_F(SessionRouterTest, RemoveSessionReleasesEveryoneAndClosesTheHandle) {
    auto session = m_host.addSession("s1");
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    ASSERT_TRUE(m_router->attachClient(2, "s1", context()));
    ASSERT_TRUE(m_router->removeSession("s1", context()));
    EXPECT_EQ(session->released.load(), 2);
    EXPECT_EQ(session->closed.load(), 1);
    EXPECT_EQ(publications().size(), 4u);
    ASSERT_TRUE(m_router->attachClient(3, "s1", context()));
    EXPECT_EQ(session->opened.load(), 2);
    EXPECT_TRUE(m_router->removeSession("unknown", context()));
}

TEST_F(SessionRouterTest, RemoveSessionReportsACloseFailure) {
    auto session = m_host.addSession("s1");
    session->closeFailure = Error{"close_failed", "stuck"};
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    EXPECT_EQ(m_router->removeSession("s1", context()).error().code, "close_failed");
}

TEST_F(SessionRouterTest, ASessionThatEndsOnItsOwnDetachesItsClients) {
    auto session = m_host.addSession("s1");
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    IRoutedSessionHandle::TerminationListener listener;
    {
        const std::lock_guard<std::mutex> lock(session->mutex);
        listener = session->listener;
    }
    ASSERT_TRUE(listener);
    listener(Error{"crashed", "worker died"});
    EXPECT_EQ(session->released.load(), 1);
    EXPECT_EQ(publications().back(), "1:none");
    EXPECT_EQ(call(1, "s1", "att1").error().code, "session_not_attached");
    EXPECT_EQ(m_errors, (std::vector<std::string>{"crashed"}));
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    EXPECT_EQ(session->opened.load(), 2);
}

TEST_F(SessionRouterTest, ReleaseWaitsForCallsInFlight) {
    auto session = m_host.addSession("s1");
    std::mutex gate;
    std::condition_variable changed;
    bool started = false;
    bool proceed = false;
    session->handler = [&](const Json&, const IServiceEndpoint::Publisher&, const ServiceContext&) {
        std::unique_lock<std::mutex> lock(gate);
        started = true;
        changed.notify_all();
        changed.wait(lock, [&] { return proceed; });
        return Result<std::optional<Json>>(std::optional<Json>(Json("done")));
    };
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    Result<std::optional<Json>> result = std::optional<Json>();
    std::thread caller([&] { result = call(1, "s1", "att1"); });
    {
        std::unique_lock<std::mutex> lock(gate);
        changed.wait(lock, [&] { return started; });
    }
    std::atomic<bool> detached{false};
    std::thread detacher([&] {
        m_router->detachClient(1, context());
        detached = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(detached.load());
    EXPECT_EQ(session->released.load(), 0);
    {
        const std::lock_guard<std::mutex> lock(gate);
        proceed = true;
    }
    changed.notify_all();
    caller.join();
    detacher.join();
    ASSERT_TRUE(result);
    EXPECT_EQ(**result, Json("done"));
    EXPECT_EQ(session->released.load(), 1);
}

TEST_F(SessionRouterTest, DrainingRefusesNewWork) {
    m_host.addSession("s1");
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    m_closing = true;
    EXPECT_EQ(m_router->attachClient(2, "s1", context()).error().code, "server_draining");
    EXPECT_EQ(call(1, "s1", "att1").error().code, "server_draining");
    EXPECT_EQ(m_router->removeSession("s1", context()).error().code, "server_draining");
}

TEST_F(SessionRouterTest, CloseReleasesAndClosesEverythingOnce) {
    auto first = m_host.addSession("s1");
    auto second = m_host.addSession("s2");
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    ASSERT_TRUE(m_router->attachClient(2, "s2", context()));
    m_closing = true;
    ASSERT_TRUE(m_router->close(context()));
    ASSERT_TRUE(m_router->close(context()));
    EXPECT_EQ(first->released.load(), 1);
    EXPECT_EQ(second->released.load(), 1);
    EXPECT_EQ(first->closed.load(), 1);
    EXPECT_EQ(second->closed.load(), 1);
}

TEST_F(SessionRouterTest, CloseReportsSessionsThatFailToClose) {
    auto session = m_host.addSession("s1");
    session->closeFailure = Error{"close_failed", "stuck"};
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    m_closing = true;
    EXPECT_EQ(m_router->close(context()).error().code, "close_failed");
    EXPECT_EQ(m_router->close(context()).error().code, "close_failed");
    EXPECT_EQ(session->closed.load(), 1);
}

TEST_F(SessionRouterTest, AttachFailureLeavesNothingBehind) {
    auto session = m_host.addSession("s1");
    ASSERT_TRUE(m_router->attachClient(1, "s1", context()));
    ASSERT_TRUE(m_router->detachClient(1, context()));
    EXPECT_EQ(session->attached.load(), session->released.load());
}
