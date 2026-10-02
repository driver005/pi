#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.thread_pool;
import pi.support.server_connection;
import pi.testing.fake_byte_connection;
import pi.testing.scripted_server_host;
import pi.testing.sequential_id_generator;

class ServerConnectionTest : public ::testing::Test {
protected:
    static constexpr const char* kServerId = "00000000-0000-4000-8000-000000000001";

    ServerConnectionTest() {
        SessionRouterOptions routerOptions;
        routerOptions.host = &m_host;
        routerOptions.ids = &m_ids;
        routerOptions.executor = &m_executor;
        routerOptions.serverId = kServerId;
        routerOptions.isClosing = [this] { return m_closing.load(); };
        routerOptions.publishAttachment = [this](std::uint64_t, const std::optional<SessionAttachment>& attachment,
                                                 const ServiceContext&) {
            if (m_connection) {
                m_connection->sendAttachment(attachment);
            }
        };
        m_router = std::make_unique<SessionRouter>(routerOptions);
        m_host.setServerHandler([this](const Json& call, const IServiceEndpoint::Publisher& publish,
                                       const ServiceContext& context) { return serverCall(call, publish, context); });
    }

    ~ServerConnectionTest() override {
        m_closing = true;
        if (m_connection) {
            m_connection->shutdown();
        }
    }

    void open(std::int64_t handshakeTimeoutMs = 5000) {
        ServerConnectionOptions options;
        options.host = &m_host;
        options.router = m_router.get();
        options.executor = &m_executor;
        options.serverId = kServerId;
        options.clientId = 1;
        options.handshakeTimeoutMs = handshakeTimeoutMs;
        options.isClosing = [this] { return m_closing.load(); };
        options.reportError = [this](const Error& error) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_errors.push_back(error.code);
        };
        options.onDisconnect = [this](std::uint64_t) { ++m_disconnects; };
        m_peer = std::make_shared<FakeByteConnection>();
        m_connection = std::make_shared<ServerConnection>(m_peer, options);
        m_peer->attach(m_connection);
    }

    Json hello(int version = 8) const {
        return Json{{"type", "hello"}, {"version", version}};
    }

    void handshake() {
        open();
        m_peer->feed(hello());
        ASSERT_TRUE(m_peer->waitForMessages(1));
    }

    Json request(const std::string& id, const Json& call, const Json& target = Json()) const {
        return Json{{"type", "request"},
                    {"id", id},
                    {"target", target.is_null() ? Json{{"serverId", kServerId}} : target},
                    {"call", call}};
    }

    Json callOf(const std::string& member, const Json& args = Json::array(), const std::string& service = "svc") const {
        return Json{{"serviceId", service}, {"member", member}, {"args", args}};
    }

    Json messageAt(std::size_t index) {
        EXPECT_TRUE(m_peer->waitForMessages(index + 1));
        const auto messages = m_peer->messages();
        return index < messages.size() ? messages[index] : Json();
    }

    Result<std::optional<Json>> serverCall(const Json& call, const IServiceEndpoint::Publisher& publish,
                                           const ServiceContext& context) {
        const std::string member = call["member"].get<std::string>();
        if (member == "echo") {
            return std::optional<Json>(call["args"]);
        }
        if (member == "fail") {
            return std::unexpected(Error{call["args"][0].get<std::string>(), "failed on purpose"});
        }
        if (member == "wait") {
            std::unique_lock<std::mutex> lock(m_gate);
            m_waiting = true;
            m_gateChanged.notify_all();
            while (!m_release && !context.signal->aborted()) {
                m_gateChanged.wait_for(lock, std::chrono::milliseconds(10));
            }
            if (context.signal->aborted()) {
                return std::unexpected(Error{"aborted", "aborted"});
            }
            return std::optional<Json>(Json("released"));
        }
        if (member == "attach") {
            auto attached = m_host.presentation()->attachSession(call["args"][0].get<std::string>(), context);
            if (!attached) {
                return std::unexpected(attached.error());
            }
            return std::optional<Json>();
        }
        if (member == "subscribe") {
            m_publish = publish;
            const std::string subscriptionId = call["args"][0].get<std::string>();
            if (m_publishBeforeSnapshot) {
                publish(subscriptionId, state(1, 2), context);
            }
            return std::optional<Json>(snapshot());
        }
        if (member == "unsubscribe") {
            return std::optional<Json>();
        }
        return std::unexpected(Error{"service_member_not_found", "no such member"});
    }

    Json snapshot() const {
        return Json::parse(
            R"({"serviceId":"svc","mode":"singleton","instances":[{"members":[{"name":"count","kind":"state","sequence":0,"ops":[["r",1]]}]}]})");
    }

    Json state(int sequence, int value) const {
        return Json{{"type", "state"}, {"member", "count"}, {"sequence", sequence}, {"ops", Json::array({Json::array({"r", value})})}};
    }

    Json subscribeCall(const std::string& subscriptionId) const {
        return Json{{"serviceId", "$chord.service"},
                    {"member", "subscribe"},
                    {"args", Json::array({subscriptionId, "svc", "singleton"})}};
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

    bool waitForWaiting() {
        std::unique_lock<std::mutex> lock(m_gate);
        return m_gateChanged.wait_for(lock, std::chrono::seconds(3), [&] { return m_waiting; });
    }

    void releaseWaiters() {
        {
            const std::lock_guard<std::mutex> lock(m_gate);
            m_release = true;
        }
        m_gateChanged.notify_all();
    }

    std::vector<std::string> errors() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_errors;
    }

    ScriptedServerHost m_host;
    SequentialIdGenerator m_ids{"att"};
    std::atomic<bool> m_closing{false};
    std::atomic<int> m_disconnects{0};
    std::mutex m_mutex;
    std::vector<std::string> m_errors;
    std::mutex m_gate;
    std::condition_variable m_gateChanged;
    bool m_waiting = false;
    bool m_release = false;
    bool m_publishBeforeSnapshot = false;
    IServiceEndpoint::Publisher m_publish;
    std::unique_ptr<SessionRouter> m_router;
    std::shared_ptr<FakeByteConnection> m_peer;
    std::shared_ptr<ServerConnection> m_connection;
    ThreadPool m_executor{4};
};

TEST_F(ServerConnectionTest, HelloIsAnsweredWithTheServerIdentity) {
    handshake();
    const Json reply = messageAt(0);
    EXPECT_EQ(reply["type"], "hello");
    EXPECT_EQ(reply["version"], 8);
    EXPECT_EQ(reply["serverId"], kServerId);
    EXPECT_EQ(m_host.serverAttachments(), 1);
    EXPECT_EQ(m_connection->stage(), ConnectionStage::Ready);
}

TEST_F(ServerConnectionTest, UnsupportedVersionsAreRefusedAndTheConnectionEnds) {
    open();
    m_peer->feed(hello(7));
    const Json reply = messageAt(0);
    EXPECT_EQ(reply["type"], "hello_error");
    EXPECT_EQ(reply["error"]["code"], "version");
    EXPECT_EQ(reply["error"]["message"], "Unsupported protocol version 7; expected 8");
    EXPECT_TRUE(m_peer->waitClosed());
    EXPECT_EQ(m_host.serverAttachments(), 0);
}

TEST_F(ServerConnectionTest, TheFirstMessageMustBeHello) {
    open();
    m_peer->feed(request("r1", callOf("echo")));
    const Json reply = messageAt(0);
    EXPECT_EQ(reply["type"], "hello_error");
    EXPECT_EQ(reply["error"]["code"], "invalid_request");
    EXPECT_EQ(reply["error"]["message"], "The first client message must be hello");
    EXPECT_TRUE(m_peer->waitClosed());
}

TEST_F(ServerConnectionTest, HelloMayOnlyBeSentOnce) {
    handshake();
    m_peer->feed(hello());
    const Json reply = messageAt(1);
    EXPECT_EQ(reply["type"], "hello_error");
    EXPECT_EQ(reply["error"]["message"], "hello may only be sent as the first message");
    EXPECT_TRUE(m_peer->waitClosed());
}

TEST_F(ServerConnectionTest, ABadFrameEndsTheConnectionWithAnError) {
    open();
    m_peer->feedRaw(std::string("\x00\x00\x00\x03\xff\xff\xff", 7));
    const Json reply = messageAt(0);
    EXPECT_EQ(reply["type"], "hello_error");
    EXPECT_EQ(reply["error"]["code"], "invalid_request");
    EXPECT_TRUE(m_peer->waitClosed());
}

TEST_F(ServerConnectionTest, ASlowHandshakeTimesOut) {
    open(20);
    m_connection->checkHandshakeTimeout();
    EXPECT_EQ(m_connection->stage(), ConnectionStage::AwaitingHello);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    m_connection->checkHandshakeTimeout();
    const Json reply = messageAt(0);
    EXPECT_EQ(reply["error"]["code"], "invalid_request");
    EXPECT_EQ(reply["error"]["message"], "Handshake timeout");
    EXPECT_TRUE(m_peer->waitClosed());
}

TEST_F(ServerConnectionTest, TheTimeoutIgnoresReadyConnections) {
    open(20);
    m_peer->feed(hello());
    ASSERT_TRUE(m_peer->waitForMessages(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    m_connection->checkHandshakeTimeout();
    EXPECT_FALSE(m_peer->closed());
}

TEST_F(ServerConnectionTest, RequestsRunServerServicesAndReturnTheirResult) {
    handshake();
    m_peer->feed(request("r1", callOf("echo", Json::array({1, "two"}))));
    const Json reply = messageAt(1);
    EXPECT_EQ(reply["type"], "response");
    EXPECT_EQ(reply["id"], "r1");
    EXPECT_EQ(reply["ok"], true);
    EXPECT_EQ(reply["result"], Json::array({1, "two"}));
}

TEST_F(ServerConnectionTest, VoidResultsOmitTheResultField) {
    handshake();
    m_peer->feed(request("r1", callOf("unsubscribe", Json::array({"x"}))));
    const Json reply = messageAt(1);
    EXPECT_EQ(reply["ok"], true);
    EXPECT_FALSE(reply.contains("result"));
}

TEST_F(ServerConnectionTest, MalformedCallsAreInvalidRequests) {
    handshake();
    m_peer->feed(request("r1", Json{{"serviceId", "svc"}}));
    const Json reply = messageAt(1);
    EXPECT_EQ(reply["ok"], false);
    EXPECT_EQ(reply["error"]["code"], "invalid_request");
    EXPECT_EQ(reply["error"]["message"], "Invalid service call");
}

TEST_F(ServerConnectionTest, RequestsForAnotherServerAreRefused) {
    handshake();
    m_peer->feed(request("r1", callOf("echo"), Json{{"serverId", "00000000-0000-4000-8000-000000000002"}}));
    const Json reply = messageAt(1);
    EXPECT_EQ(reply["error"]["code"], "wrong_server");
}

TEST_F(ServerConnectionTest, KnownErrorCodesCrossTheWireAndOthersAreHidden) {
    handshake();
    m_peer->feed(request("r1", callOf("fail", Json::array({"service_not_found"}))));
    EXPECT_EQ(messageAt(1)["error"]["code"], "service_not_found");
    EXPECT_EQ(messageAt(1)["error"]["message"], "failed on purpose");
    m_peer->feed(request("r2", callOf("fail", Json::array({"database_exploded"}))));
    const Json hidden = messageAt(2);
    EXPECT_EQ(hidden["error"]["code"], "internal_error");
    EXPECT_EQ(hidden["error"]["message"], "Internal server error");
    EXPECT_EQ(errors(), (std::vector<std::string>{"database_exploded"}));
}

TEST_F(ServerConnectionTest, DuplicateRequestIdsAreRefusedWhileActive) {
    handshake();
    m_peer->feed(request("r1", callOf("wait")));
    ASSERT_TRUE(waitForWaiting());
    m_peer->feed(request("r1", callOf("echo")));
    const Json refusal = messageAt(1);
    EXPECT_EQ(refusal["id"], "r1");
    EXPECT_EQ(refusal["error"]["message"], "Request ID is already active");
    releaseWaiters();
    const Json done = messageAt(2);
    EXPECT_EQ(done["result"], "released");
    m_peer->feed(request("r1", callOf("echo")));
    EXPECT_EQ(messageAt(3)["ok"], true);
}

TEST_F(ServerConnectionTest, CancelAbortsTheRequestAndAnswersCancelled) {
    handshake();
    m_peer->feed(request("r1", callOf("wait")));
    ASSERT_TRUE(waitForWaiting());
    m_peer->feed(Json{{"type", "cancel"}, {"id", "r1"}, {"target", Json{{"serverId", kServerId}}}});
    const Json reply = messageAt(1);
    EXPECT_EQ(reply["id"], "r1");
    EXPECT_EQ(reply["ok"], false);
    EXPECT_EQ(reply["error"]["code"], "cancelled");
}

TEST_F(ServerConnectionTest, CancelForAnotherTargetDoesNothing) {
    handshake();
    m_peer->feed(request("r1", callOf("wait")));
    ASSERT_TRUE(waitForWaiting());
    m_peer->feed(Json{{"type", "cancel"},
                      {"id", "r1"},
                      {"target", Json{{"serverId", "00000000-0000-4000-8000-000000000002"}}}});
    m_peer->feed(Json{{"type", "cancel"}, {"id", "unknown"}, {"target", Json{{"serverId", kServerId}}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(m_peer->messages().size(), 1u);
    releaseWaiters();
    EXPECT_EQ(messageAt(1)["result"], "released");
}

TEST_F(ServerConnectionTest, SubscriptionSnapshotsAreWireEncodedAndUpdatesFollow) {
    handshake();
    m_peer->feed(request("r1", subscribeCall("sub1")));
    const Json reply = messageAt(1);
    EXPECT_EQ(reply["ok"], true);
    EXPECT_EQ(reply["result"]["serviceId"], "svc");
    EXPECT_EQ(reply["result"]["instances"][0]["members"][0]["ops"][0][0], "r");
    m_publish("sub1", state(1, 2), ServiceContext{});
    const Json update = messageAt(2);
    EXPECT_EQ(update["type"], "service_update");
    EXPECT_EQ(update["subscriptionId"], "sub1");
    EXPECT_EQ(update["update"]["type"], "state");
    EXPECT_EQ(update["update"]["sequence"], 1);
}

TEST_F(ServerConnectionTest, UpdatesBeforeTheSnapshotResponseAreSentAfterIt) {
    m_publishBeforeSnapshot = true;
    handshake();
    m_peer->feed(request("r1", subscribeCall("sub1")));
    EXPECT_EQ(messageAt(1)["type"], "response");
    const Json update = messageAt(2);
    EXPECT_EQ(update["type"], "service_update");
    EXPECT_EQ(update["update"]["sequence"], 1);
}

TEST_F(ServerConnectionTest, UpdatesForUnknownSubscriptionsAreDropped) {
    handshake();
    m_peer->feed(request("r1", subscribeCall("sub1")));
    ASSERT_TRUE(m_peer->waitForMessages(2));
    m_peer->feed(request("r2", Json{{"serviceId", "$chord.service"}, {"member", "unsubscribe"}, {"args", Json::array({"sub1"})}}));
    ASSERT_TRUE(m_peer->waitForMessages(3));
    m_publish("sub1", state(1, 2), ServiceContext{});
    m_publish("other", state(1, 2), ServiceContext{});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(m_peer->messages().size(), 3u);
}

TEST_F(ServerConnectionTest, DuplicateSubscriptionIdsAreRefused) {
    handshake();
    m_peer->feed(request("r1", subscribeCall("sub1")));
    ASSERT_TRUE(m_peer->waitForMessages(2));
    m_peer->feed(request("r2", subscribeCall("sub1")));
    const Json reply = messageAt(2);
    EXPECT_EQ(reply["error"]["code"], "invalid_request");
    EXPECT_EQ(reply["error"]["message"], "Duplicate service subscription sub1");
}

TEST_F(ServerConnectionTest, AttachingASessionAnnouncesItAndRoutesItsRequests) {
    auto session = m_host.addSession("s1");
    session->handler = [](const Json& call, const IServiceEndpoint::Publisher&, const ServiceContext&) {
        return Result<std::optional<Json>>(std::optional<Json>(Json("session " + call["member"].get<std::string>())));
    };
    handshake();
    m_peer->feed(request("r1", callOf("attach", Json::array({"s1"}))));
    const Json attachment = messageAt(1);
    EXPECT_EQ(attachment["type"], "attachment");
    EXPECT_EQ(attachment["attachment"]["sessionId"], "s1");
    EXPECT_EQ(attachment["attachment"]["serverId"], kServerId);
    EXPECT_EQ(messageAt(2)["id"], "r1");
    m_peer->feed(request("r2", callOf("hi"), attachment["attachment"]));
    const Json reply = messageAt(3);
    EXPECT_EQ(reply["ok"], true);
    EXPECT_EQ(reply["result"], "session hi");
}

TEST_F(ServerConnectionTest, SessionRequestsWithAStaleAttachmentAreRefused) {
    m_host.addSession("s1");
    handshake();
    const Json target = {{"serverId", kServerId}, {"sessionId", "s1"}, {"attachmentId", "att9"}};
    m_peer->feed(request("r1", callOf("hi"), target));
    EXPECT_EQ(messageAt(1)["error"]["code"], "session_not_attached");
}

TEST_F(ServerConnectionTest, HangingUpAbortsWorkAndReleasesEverything) {
    auto session = m_host.addSession("s1");
    handshake();
    m_peer->feed(request("r1", callOf("attach", Json::array({"s1"}))));
    ASSERT_TRUE(m_peer->waitForMessages(3));
    m_peer->feed(request("r2", callOf("wait")));
    ASSERT_TRUE(waitForWaiting());
    m_peer->hangUp();
    EXPECT_TRUE(waitUntil([&] { return m_host.serverReleases() == 1 && session->released.load() == 1; }));
    EXPECT_EQ(m_disconnects.load(), 1);
    EXPECT_EQ(m_connection->stage(), ConnectionStage::Closed);
}

TEST_F(ServerConnectionTest, ASendFailureDisconnects) {
    handshake();
    m_peer->failSends(true);
    m_peer->feed(request("r1", callOf("echo")));
    EXPECT_TRUE(waitUntil([&] { return m_disconnects.load() == 1; }));
    EXPECT_TRUE(waitUntil([&] { return m_host.serverReleases() == 1; }));
    EXPECT_FALSE(errors().empty());
}

TEST_F(ServerConnectionTest, ShutdownEndsTheConnection) {
    handshake();
    m_connection->shutdown();
    EXPECT_TRUE(m_peer->closed());
    EXPECT_TRUE(waitUntil([&] { return m_host.serverReleases() == 1; }));
    m_peer->feed(request("r1", callOf("echo")));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(m_peer->messages().size(), 1u);
}

TEST_F(ServerConnectionTest, HostFailuresDuringTheHandshakeAreReported) {
    m_host.failServerAttach(Error{"server_draining", "Server is draining"});
    open();
    m_peer->feed(hello());
    const Json reply = messageAt(0);
    EXPECT_EQ(reply["type"], "hello_error");
    EXPECT_EQ(reply["error"]["code"], "server_draining");
    EXPECT_TRUE(m_peer->waitClosed());
}

TEST_F(ServerConnectionTest, ClosingServersEndHandshakesWithoutAnswering) {
    open();
    m_closing = true;
    m_peer->feed(hello());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_TRUE(m_peer->messages().empty());
    EXPECT_EQ(m_host.serverAttachments(), 0);
}
