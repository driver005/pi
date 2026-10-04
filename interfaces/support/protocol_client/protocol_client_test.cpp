#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.protocol_client;
import pi.testing.fake_client_transport;

class ProtocolClientTest : public testing::Test {
protected:
    ProtocolClientTest()
        : m_client(m_transport, kServerId) {}

    /** Answers hello, and requests through `answer` (a response body, or null to stay silent). */
    void serve(const std::function<Json(const Json& request)>& answer) {
        m_transport.respondWith([this, answer](const Json& message) {
            if (message["type"] == "hello") {
                m_transport.push(Json{{"type", "hello"}, {"version", 8}, {"serverId", kServerId}});
            } else if (message["type"] == "request") {
                const Json body = answer(message);
                if (!body.is_null()) {
                    Json response = Json{{"type", "response"}, {"id", message["id"]}};
                    for (const auto& entry : body.items()) {
                        response[entry.key()] = entry.value();
                    }
                    m_transport.push(response);
                }
            }
        });
    }

    Json okResult(const Json& result) {
        return Json{{"ok", true}, {"result", result}};
    }

    template <typename Predicate>
    bool eventually(const Predicate& predicate) {
        for (int i = 0; i < 500; ++i) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    static constexpr const char* kServerId = "123e4567-e89b-42d3-a456-426614174000";
    FakeClientTransport m_transport;
    ProtocolClient m_client;
};

TEST_F(ProtocolClientTest, ConnectsAndReturnsTheServerHello) {
    serve([](const Json&) { return Json(); });
    const auto hello = m_client.connect();
    ASSERT_TRUE(hello.has_value()) << hello.error().message;
    EXPECT_EQ(hello->at("serverId"), kServerId);
    EXPECT_TRUE(m_client.connected());
    EXPECT_EQ(m_client.hello(), std::optional<Json>(*hello));
    EXPECT_EQ(m_transport.received()[0], (Json{{"type", "hello"}, {"version", 8}}));
}

TEST_F(ProtocolClientTest, RefusesAServerWithAnotherId) {
    m_transport.respondWith([this](const Json&) {
        m_transport.push(Json{{"type", "hello"}, {"version", 8}, {"serverId", "223e4567-e89b-42d3-a456-426614174000"}});
    });
    const auto hello = m_client.connect();
    ASSERT_FALSE(hello.has_value());
    EXPECT_EQ(hello.error().code, "protocol_validation");
    EXPECT_FALSE(m_client.connected());
}

TEST_F(ProtocolClientTest, ReportsAHelloErrorWithTheServersCode) {
    m_transport.respondWith([this](const Json&) {
        m_transport.push(Json{{"type", "hello_error"}, {"error", Json{{"code", "unsupported_version"}, {"message", "too old"}}}});
    });
    const auto hello = m_client.connect();
    ASSERT_FALSE(hello.has_value());
    EXPECT_EQ(hello.error().code, "unsupported_version");
    EXPECT_EQ(hello.error().message, "too old");
}

TEST_F(ProtocolClientTest, FailsWhenTheTransportRefuses) {
    m_transport.refuseConnections();
    const auto hello = m_client.connect();
    ASSERT_FALSE(hello.has_value());
    EXPECT_EQ(hello.error().code, "disconnected");
}

TEST_F(ProtocolClientTest, TimesOutWithoutAServerHello) {
    FakeClientTransport silent;
    ProtocolClient client(silent, kServerId, 1024 * 1024, 50);
    const auto hello = client.connect();
    ASSERT_FALSE(hello.has_value());
    EXPECT_EQ(hello.error().code, "handshake_timeout");
}

TEST_F(ProtocolClientTest, RequestsReturnTheResultAndServerErrorsKeepTheirCode) {
    serve([this](const Json& request) {
        if (request["call"]["member"] == "boom") {
            return Json{{"ok", false}, {"error", Json{{"code", "bad_thing"}, {"message", "it broke"}}}};
        }
        return okResult(request["call"]["args"]);
    });
    ASSERT_TRUE(m_client.connect().has_value());
    const Json call{{"serviceId", "s"}, {"member", "echo"}, {"args", Json::array({1, "two"})}};
    const auto answered = m_client.request(m_client.serverTarget(), call);
    ASSERT_TRUE(answered.has_value());
    EXPECT_EQ(*answered, std::optional<Json>(Json::array({1, "two"})));
    const auto failed = m_client.request(m_client.serverTarget(), Json{{"serviceId", "s"}, {"member", "boom"}, {"args", Json::array()}});
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, "bad_thing");
    EXPECT_EQ(failed.error().message, "it broke");
    EXPECT_TRUE(m_client.connected());
}

TEST_F(ProtocolClientTest, ARequestWithoutAResultYieldsNothing) {
    serve([](const Json&) { return Json{{"ok", true}}; });
    ASSERT_TRUE(m_client.connect().has_value());
    const auto answered = m_client.request(m_client.serverTarget(), Json{{"serviceId", "s"}, {"member", "m"}, {"args", Json::array()}});
    ASSERT_TRUE(answered.has_value());
    EXPECT_FALSE(answered->has_value());
}

TEST_F(ProtocolClientTest, InvalidCallsAreRejectedBeforeSending) {
    serve([](const Json&) { return Json(); });
    ASSERT_TRUE(m_client.connect().has_value());
    const auto answered = m_client.request(m_client.serverTarget(), Json{{"serviceId", "s"}});
    ASSERT_FALSE(answered.has_value());
    EXPECT_EQ(answered.error().code, "invalid_request");
    EXPECT_EQ(m_transport.received().size(), 1u);
}

TEST_F(ProtocolClientTest, RequestsFailWhileDisconnected) {
    const auto answered = m_client.request(m_client.serverTarget(), Json{{"serviceId", "s"}, {"member", "m"}, {"args", Json::array()}});
    ASSERT_FALSE(answered.has_value());
    EXPECT_EQ(answered.error().code, "disconnected");
}

TEST_F(ProtocolClientTest, AbortingARequestFailsItAtOnceAndTellsTheServerToCancel) {
    serve([](const Json&) { return Json(); });
    ASSERT_TRUE(m_client.connect().has_value());
    const auto signal = std::make_shared<AbortSignal>();
    std::thread aborter([&] {
        m_transport.waitForMessages(2);
        signal->abort();
    });
    const auto answered = m_client.request(m_client.serverTarget(), Json{{"serviceId", "s"}, {"member", "slow"}, {"args", Json::array()}}, signal);
    aborter.join();
    ASSERT_FALSE(answered.has_value());
    EXPECT_EQ(answered.error().code, "aborted");
    const std::vector<Json> sent = m_transport.waitForMessages(3);
    ASSERT_EQ(sent.size(), 3u);
    EXPECT_EQ(sent[2]["type"], "cancel");
    EXPECT_EQ(sent[2]["id"], sent[1]["id"]);
    // The late response is expected, not a protocol violation.
    m_transport.push(Json{{"type", "response"}, {"id", sent[1]["id"]}, {"ok", true}});
    EXPECT_TRUE(m_client.connected());
}

TEST_F(ProtocolClientTest, AnAlreadyAbortedSignalSendsNothing) {
    serve([](const Json&) { return Json(); });
    ASSERT_TRUE(m_client.connect().has_value());
    const auto signal = std::make_shared<AbortSignal>();
    signal->abort();
    const auto answered = m_client.request(m_client.serverTarget(), Json{{"serviceId", "s"}, {"member", "m"}, {"args", Json::array()}}, signal);
    ASSERT_FALSE(answered.has_value());
    EXPECT_EQ(answered.error().code, "aborted");
    EXPECT_EQ(m_transport.received().size(), 1u);
}

TEST_F(ProtocolClientTest, AResponseWithoutARequestDisconnects) {
    serve([](const Json&) { return Json(); });
    ASSERT_TRUE(m_client.connect().has_value());
    m_transport.push(Json{{"type", "response"}, {"id", "request-99"}, {"ok", true}});
    EXPECT_TRUE(eventually([&] { return !m_client.connected(); }));
}

TEST_F(ProtocolClientTest, GarbageBytesDisconnect) {
    serve([](const Json&) { return Json(); });
    ASSERT_TRUE(m_client.connect().has_value());
    m_transport.pushBytes(std::string("\x00\x00\x00\x03\xff\xff\xff", 7));
    EXPECT_TRUE(eventually([&] { return !m_client.connected(); }));
}

TEST_F(ProtocolClientTest, LosingTheTransportFailsPendingRequestsAndNotifiesTheListener) {
    serve([](const Json&) { return Json(); });
    ASSERT_TRUE(m_client.connect().has_value());
    std::mutex mutex;
    std::optional<Error> reported;
    m_client.onDisconnect([&](const Error& error) {
        const std::lock_guard<std::mutex> lock(mutex);
        reported = error;
    });
    std::thread killer([&] {
        m_transport.waitForMessages(2);
        m_transport.remoteClose();
    });
    const auto answered = m_client.request(m_client.serverTarget(), Json{{"serviceId", "s"}, {"member", "m"}, {"args", Json::array()}});
    killer.join();
    ASSERT_FALSE(answered.has_value());
    EXPECT_EQ(answered.error().code, "disconnected");
    ASSERT_TRUE(eventually([&] {
        const std::lock_guard<std::mutex> lock(mutex);
        return reported.has_value();
    }));
    EXPECT_FALSE(m_client.connected());
}

TEST_F(ProtocolClientTest, ReconnectsAfterADisconnect) {
    serve([](const Json&) { return Json(); });
    ASSERT_TRUE(m_client.connect().has_value());
    m_client.disconnect();
    ASSERT_TRUE(m_client.connect().has_value());
    EXPECT_TRUE(m_client.connected());
    EXPECT_EQ(m_transport.connects(), 2);
}

TEST_F(ProtocolClientTest, TracksTheAttachmentTheServerReports) {
    serve([](const Json&) { return Json(); });
    std::mutex mutex;
    std::vector<Json> seen;
    m_client.onAttachment([&](const Json& attachment) {
        const std::lock_guard<std::mutex> lock(mutex);
        seen.push_back(attachment);
    });
    ASSERT_TRUE(m_client.connect().has_value());
    const Json target{{"serverId", kServerId}, {"sessionId", "s1"}, {"attachmentId", "a1"}};
    m_transport.push(Json{{"type", "attachment"}, {"attachment", target}});
    EXPECT_EQ(m_client.attachment(), std::optional<Json>(target));
    m_transport.push(Json{{"type", "attachment"}, {"attachment", target}});
    m_transport.push(Json{{"type", "attachment"}, {"attachment", nullptr}});
    EXPECT_FALSE(m_client.attachment().has_value());
    ASSERT_TRUE(eventually([&] {
        const std::lock_guard<std::mutex> lock(mutex);
        return seen.size() == 2;
    }));
    const std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(seen[0], target);
    EXPECT_TRUE(seen[1].is_null());
}

TEST_F(ProtocolClientTest, AnAttachmentOfAnotherServerDisconnects) {
    serve([](const Json&) { return Json(); });
    ASSERT_TRUE(m_client.connect().has_value());
    m_transport.push(Json{{"type", "attachment"}, {"attachment", Json{{"serverId", "223e4567-e89b-42d3-a456-426614174000"}, {"sessionId", "s"}, {"attachmentId", "a"}}}});
    EXPECT_TRUE(eventually([&] { return !m_client.connected(); }));
}

TEST_F(ProtocolClientTest, ReadsTheServiceCatalogue) {
    serve([this](const Json&) { return okResult(Json::parse(R"([{"serviceId":"pi.models","mode":"singleton"}])")); });
    ASSERT_TRUE(m_client.connect().has_value());
    const auto catalogue = m_client.serviceCatalogue(m_client.serverTarget());
    ASSERT_TRUE(catalogue.has_value());
    EXPECT_EQ((*catalogue)[0]["serviceId"], "pi.models");
}

TEST_F(ProtocolClientTest, AMalformedCatalogueDisconnects) {
    serve([this](const Json&) { return okResult(Json::parse(R"([{"serviceId":"x"}])")); });
    ASSERT_TRUE(m_client.connect().has_value());
    const auto catalogue = m_client.serviceCatalogue(m_client.serverTarget());
    ASSERT_FALSE(catalogue.has_value());
    EXPECT_FALSE(m_client.connected());
}

class ProtocolClientSubscriptionTest : public ProtocolClientTest {
protected:
    /** A server whose subscribe answers `snapshot` after pushing `before` updates, and which acknowledges unsubscribe. */
    void serveSubscription(const std::vector<Json>& before) {
        serve([this, before](const Json& request) {
            const Json& call = request["call"];
            if (call["member"] == "subscribe") {
                const std::string subscription = call["args"][0].get<std::string>();
                for (const Json& update : before) {
                    m_transport.push(Json{{"type", "service_update"}, {"subscriptionId", subscription}, {"update", update}});
                }
                return okResult(Json::parse(R"({"serviceId":"s","mode":"singleton","instances":[{"members":[{"name":"state","kind":"state","sequence":0,"ops":[["r",{"n":0}]]}]}]})"));
            }
            m_unsubscribed = call["args"][0].get<std::string>();
            return Json{{"ok", true}};
        });
    }

    Json stateUpdate(int sequence) {
        return Json{{"type", "state"}, {"member", "state"}, {"sequence", sequence}, {"ops", Json::array({Json::array({"s", Json::array({"n"}), sequence})})}};
    }

    std::string m_unsubscribed;
};

TEST_F(ProtocolClientSubscriptionTest, SubscribingReturnsTheDecodedSnapshot) {
    serveSubscription({});
    ASSERT_TRUE(m_client.connect().has_value());
    const auto subscription = m_client.subscribeService(m_client.serverTarget(), "s", "singleton", [](const Json&) {});
    ASSERT_TRUE(subscription.has_value()) << subscription.error().message;
    EXPECT_EQ(subscription->snapshot["instances"][0]["members"][0]["ops"], Json::parse(R"([["r",{"n":0}]])"));
    EXPECT_EQ(m_transport.received()[1]["call"]["args"][1], "s");
}

TEST_F(ProtocolClientSubscriptionTest, UpdatesWaitForStartAndArriveDecodedInOrder) {
    serveSubscription({stateUpdate(1)});
    ASSERT_TRUE(m_client.connect().has_value());
    std::mutex mutex;
    std::vector<Json> updates;
    const auto subscription = m_client.subscribeService(m_client.serverTarget(), "s", "singleton", [&](const Json& update) {
        const std::lock_guard<std::mutex> lock(mutex);
        updates.push_back(update);
    });
    ASSERT_TRUE(subscription.has_value());
    // An update that arrived before the snapshot is held too.
    m_transport.push(Json{{"type", "service_update"}, {"subscriptionId", subscription->id}, {"update", stateUpdate(2)}});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        const std::lock_guard<std::mutex> lock(mutex);
        EXPECT_TRUE(updates.empty());
    }
    m_client.start(subscription->id);
    m_transport.push(Json{{"type", "service_update"}, {"subscriptionId", subscription->id}, {"update", stateUpdate(3)}});
    ASSERT_TRUE(eventually([&] {
        const std::lock_guard<std::mutex> lock(mutex);
        return updates.size() == 3;
    }));
    const std::lock_guard<std::mutex> lock(mutex);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(updates[i]["sequence"], i + 1);
        EXPECT_EQ(updates[i]["ops"][0][2], i + 1);
    }
}

TEST_F(ProtocolClientSubscriptionTest, DisposingStopsDeliveryAndUnsubscribes) {
    serveSubscription({});
    ASSERT_TRUE(m_client.connect().has_value());
    std::atomic<int> delivered{0};
    const auto subscription = m_client.subscribeService(m_client.serverTarget(), "s", "singleton", [&](const Json&) { ++delivered; });
    ASSERT_TRUE(subscription.has_value());
    m_client.start(subscription->id);
    ASSERT_TRUE(m_client.dispose(subscription->id).has_value());
    EXPECT_EQ(m_unsubscribed, subscription->id);
    m_transport.push(Json{{"type", "service_update"}, {"subscriptionId", subscription->id}, {"update", stateUpdate(1)}});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(delivered.load(), 0);
}

TEST_F(ProtocolClientSubscriptionTest, AnUpdateForAnUnknownStateDisconnects) {
    serveSubscription({});
    ASSERT_TRUE(m_client.connect().has_value());
    const auto subscription = m_client.subscribeService(m_client.serverTarget(), "s", "singleton", [](const Json&) {});
    ASSERT_TRUE(subscription.has_value());
    m_transport.push(Json{{"type", "service_update"}, {"subscriptionId", subscription->id},
                          {"update", Json{{"type", "state"}, {"member", "nope"}, {"sequence", 1}, {"ops", Json::array()}}}});
    EXPECT_TRUE(eventually([&] { return !m_client.connected(); }));
}

TEST_F(ProtocolClientSubscriptionTest, ASubscribeErrorLeavesNoSubscriptionBehind) {
    serve([](const Json&) { return Json{{"ok", false}, {"error", Json{{"code", "unknown_service"}, {"message", "no such service"}}}}; });
    ASSERT_TRUE(m_client.connect().has_value());
    const auto subscription = m_client.subscribeService(m_client.serverTarget(), "missing", "singleton", [](const Json&) {});
    ASSERT_FALSE(subscription.has_value());
    EXPECT_EQ(subscription.error().code, "unknown_service");
    EXPECT_TRUE(m_client.connected());
}
