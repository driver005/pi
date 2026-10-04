#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.thread_pool;
import pi.server.server;
import pi.testing.fake_server_listener;
import pi.testing.scripted_server_host;
import pi.testing.sequential_id_generator;

class ServerTest : public ::testing::Test {
protected:
    static constexpr const char* kServerId = "00000000-0000-4000-8000-000000000001";

    ServerTest() {
        m_host.setServerHandler([this](const Json& call, const IServiceEndpoint::Publisher&, const ServiceContext& context) {
            return serverCall(call, context);
        });
    }

    ~ServerTest() override {
        if (m_server) {
            m_server->close();
        }
    }

    ServerOptions options(std::int64_t handshakeTimeoutMs = 5000) {
        ServerOptions result;
        result.serverId = kServerId;
        result.handshakeTimeoutMs = handshakeTimeoutMs;
        result.onConnectionCountChanged = [this](std::size_t count) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_counts.push_back(count);
        };
        result.onError = [this](const Error& error) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_errors.push_back(error.code);
        };
        return result;
    }

    void build(const ServerOptions& serverOptions, std::vector<IServerListener*> listeners) {
        m_server = std::make_unique<Server>(m_host, m_executor, m_ids, std::move(listeners), serverOptions);
    }

    void startServer(std::int64_t handshakeTimeoutMs = 5000) {
        build(options(handshakeTimeoutMs), {&m_listener});
        ASSERT_TRUE(m_server->start());
    }

    std::shared_ptr<FakeByteConnection> client() {
        auto connection = m_listener.connect();
        EXPECT_TRUE(connection != nullptr);
        connection->feed(Json{{"type", "hello"}, {"version", 8}});
        EXPECT_TRUE(connection->waitForMessages(1));
        return connection;
    }

    Result<std::optional<Json>> serverCall(const Json& call, const ServiceContext& context) {
        const std::string member = call["member"].get<std::string>();
        if (member == "attach") {
            auto attached = m_host.presentation()->attachSession(call["args"][0].get<std::string>(), context);
            if (!attached) {
                return std::unexpected(attached.error());
            }
            return std::optional<Json>();
        }
        return std::optional<Json>(call["args"]);
    }

    Json request(const std::string& id, const std::string& member, const Json& args, const Json& target = Json()) const {
        return Json{{"type", "request"},
                    {"id", id},
                    {"target", target.is_null() ? Json{{"serverId", kServerId}} : target},
                    {"call", Json{{"serviceId", "svc"}, {"member", member}, {"args", args}}}};
    }

    bool waitUntil(const std::function<bool()>& condition) const {
        for (int i = 0; i < 300; ++i) {
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return condition();
    }

    std::vector<std::size_t> counts() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_counts;
    }

    ScriptedServerHost m_host;
    SequentialIdGenerator m_ids{"att"};
    FakeServerListener m_listener;
    FakeServerListener m_secondListener;
    std::mutex m_mutex;
    std::vector<std::size_t> m_counts;
    std::vector<std::string> m_errors;
    std::unique_ptr<Server> m_server;
    ThreadPool m_executor{4};
};

TEST_F(ServerTest, ServesClientsThroughItsListeners) {
    startServer();
    EXPECT_TRUE(m_listener.started());
    auto connection = client();
    EXPECT_EQ(connection->messages()[0]["serverId"], kServerId);
    connection->feed(request("r1", "echo", Json::array({1})));
    ASSERT_TRUE(connection->waitForMessages(2));
    EXPECT_EQ(connection->messages()[1]["result"], Json::array({1}));
    EXPECT_EQ(m_server->serverId(), kServerId);
}

TEST_F(ServerTest, ReportsTheConnectionCount) {
    startServer();
    auto first = client();
    auto second = client();
    EXPECT_EQ(counts(), (std::vector<std::size_t>{1, 2}));
    first->hangUp();
    EXPECT_TRUE(waitUntil([&] { return counts().size() == 3; }));
    EXPECT_EQ(counts().back(), 1u);
    second->hangUp();
    EXPECT_TRUE(waitUntil([&] { return counts().size() == 4; }));
    EXPECT_EQ(counts().back(), 0u);
}

TEST_F(ServerTest, AttachedSessionsAreServedAndAnnounced) {
    auto session = m_host.addSession("s1");
    session->handler = [](const Json&, const IServiceEndpoint::Publisher&, const ServiceContext&) {
        return Result<std::optional<Json>>(std::optional<Json>(Json("from session")));
    };
    startServer();
    auto connection = client();
    connection->feed(request("r1", "attach", Json::array({"s1"})));
    ASSERT_TRUE(connection->waitForMessages(3));
    const Json attachment = connection->messages()[1];
    EXPECT_EQ(attachment["type"], "attachment");
    connection->feed(request("r2", "anything", Json::array(), attachment["attachment"]));
    ASSERT_TRUE(connection->waitForMessages(4));
    EXPECT_EQ(connection->messages()[3]["result"], "from session");
}

TEST_F(ServerTest, ClientsShareAnOpenedSession) {
    auto session = m_host.addSession("s1");
    startServer();
    auto first = client();
    first->feed(request("r1", "attach", Json::array({"s1"})));
    ASSERT_TRUE(first->waitForMessages(3));
    auto second = client();
    second->feed(request("r1", "attach", Json::array({"s1"})));
    ASSERT_TRUE(second->waitForMessages(3));
    EXPECT_EQ(session->opened.load(), 1);
    EXPECT_EQ(session->attached.load(), 2);
    EXPECT_NE(first->messages()[1]["attachment"]["attachmentId"], second->messages()[1]["attachment"]["attachmentId"]);
}

TEST_F(ServerTest, DisconnectedClientsReleaseTheirSession) {
    auto session = m_host.addSession("s1");
    startServer();
    auto connection = client();
    connection->feed(request("r1", "attach", Json::array({"s1"})));
    ASSERT_TRUE(connection->waitForMessages(3));
    connection->hangUp();
    EXPECT_TRUE(waitUntil([&] { return session->released.load() == 1 && m_host.serverReleases() == 1; }));
    EXPECT_EQ(session->closed.load(), 0);
}

TEST_F(ServerTest, ClientsThatNeverSayHelloAreCut) {
    startServer(30);
    auto connection = m_listener.connect();
    ASSERT_TRUE(connection != nullptr);
    ASSERT_TRUE(connection->waitForMessages(1));
    EXPECT_EQ(connection->messages()[0]["error"]["message"], "Handshake timeout");
    EXPECT_TRUE(connection->waitClosed());
    EXPECT_TRUE(waitUntil([&] { return counts().back() == 0; }));
}

TEST_F(ServerTest, CloseDisconnectsClientsAndClosesSessions) {
    auto session = m_host.addSession("s1");
    startServer();
    auto connection = client();
    connection->feed(request("r1", "attach", Json::array({"s1"})));
    ASSERT_TRUE(connection->waitForMessages(3));
    ASSERT_TRUE(m_server->close());
    EXPECT_TRUE(m_listener.closedByServer());
    EXPECT_TRUE(connection->closed());
    EXPECT_EQ(session->closed.load(), 1);
    EXPECT_EQ(session->released.load(), 1);
    EXPECT_TRUE(waitUntil([&] { return m_host.serverReleases() == 1; }));
    EXPECT_TRUE(m_server->close());
}

TEST_F(ServerTest, ConnectionsAcceptedWhileClosingAreEnded) {
    startServer();
    ASSERT_TRUE(m_server->close());
    EXPECT_EQ(m_listener.connect(), nullptr);
}

TEST_F(ServerTest, InvalidOptionsAreRefusedAtStart) {
    ServerOptions bad = options();
    bad.serverId = "not-a-uuid";
    build(bad, {&m_listener});
    EXPECT_EQ(m_server->start().error().code, "invalid_options");
    EXPECT_FALSE(m_listener.started());
    ServerOptions frames = options();
    frames.maxFrameLength = 0;
    build(frames, {&m_listener});
    EXPECT_EQ(m_server->start().error().code, "invalid_options");
    ServerOptions timeout = options();
    timeout.handshakeTimeoutMs = 0;
    build(timeout, {&m_listener});
    EXPECT_EQ(m_server->start().error().code, "invalid_options");
}

TEST_F(ServerTest, StartingTwiceIsAnError) {
    startServer();
    EXPECT_EQ(m_server->start().error().code, "invalid_state");
}

TEST_F(ServerTest, AListenerThatFailsToStartStopsTheOthers) {
    m_secondListener.failStart(Error{"bind_failed", "address in use"});
    build(options(), {&m_listener, &m_secondListener});
    EXPECT_EQ(m_server->start().error().code, "bind_failed");
    EXPECT_TRUE(m_listener.closedByServer());
    EXPECT_EQ(m_server->start().error().code, "invalid_state");
}

TEST_F(ServerTest, RequestsForUnknownSessionsFailCleanly) {
    startServer();
    auto connection = client();
    connection->feed(request("r1", "attach", Json::array({"missing"})));
    ASSERT_TRUE(connection->waitForMessages(2));
    EXPECT_EQ(connection->messages()[1]["error"]["code"], "session_not_found");
}

TEST_F(ServerTest, ASessionThatEndsOnItsOwnIsAnnouncedAsDetached) {
    auto session = m_host.addSession("s1");
    startServer();
    auto connection = client();
    connection->feed(request("r1", "attach", Json::array({"s1"})));
    ASSERT_TRUE(connection->waitForMessages(3));
    IRoutedSessionHandle::TerminationListener listener;
    {
        const std::lock_guard<std::mutex> lock(session->mutex);
        listener = session->listener;
    }
    ASSERT_TRUE(listener);
    listener(Error{"crashed", "worker died"});
    ASSERT_TRUE(connection->waitForMessages(4));
    EXPECT_EQ(connection->messages()[3]["type"], "attachment");
    EXPECT_TRUE(connection->messages()[3]["attachment"].is_null());
}
