#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.chord.remote_service_endpoint;
import pi.chord.remote_service_provider;
import pi.chord.replicated_state;

class CounterService : public IRemoteService {
public:
    CounterService() : m_state(Json{{"n", 0}}) {}

    std::map<std::string, Method> methods() override {
        return {{"bump", [this](const std::vector<Json>&, const ServiceContext& context) -> Result<std::optional<Json>> {
                     m_state.change(context, [](Json& draft) { draft["n"] = draft["n"].get<int>() + 1; });
                     return std::optional<Json>(m_state.value()["n"]);
                 }}};
    }

    std::map<std::string, IReplicatedState*> states() override {
        return {{"state", &m_state}};
    }

    ReplicatedState m_state;
};

class RemoteServiceEndpointTest : public testing::Test {
protected:
    RemoteServiceEndpointTest() : m_provider({ServiceDefinition{"pi.counter", "singleton"}}), m_endpoint(m_provider) {
        m_service = std::make_shared<CounterService>();
        m_provider.provide("pi.counter", m_service);
        m_publish = [this](const std::string& subscriptionId, const Json& update, const ServiceContext&) {
            m_published.emplace_back(subscriptionId, update);
        };
    }

    Result<std::optional<Json>> invoke(const Json& call) {
        return m_endpoint.invoke(call, m_publish, ServiceContext{});
    }

    ServiceWire m_wire;
    RemoteServiceProvider m_provider;
    RemoteServiceEndpoint m_endpoint;
    std::shared_ptr<CounterService> m_service;
    IServiceEndpoint::Publisher m_publish;
    std::vector<std::pair<std::string, Json>> m_published;
};

TEST_F(RemoteServiceEndpointTest, AnswersTheCatalogue) {
    const auto result = invoke(m_wire.catalogueCall());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(**result, Json::parse(R"([{"serviceId":"pi.counter","mode":"singleton"}])"));
}

TEST_F(RemoteServiceEndpointTest, ForwardsOrdinaryCalls) {
    const auto result = invoke(Json{{"serviceId", "pi.counter"}, {"member", "bump"}, {"args", Json::array()}});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(**result, 1);
    EXPECT_EQ(invoke(Json{{"serviceId", "pi.nope"}, {"member", "bump"}, {"args", Json::array()}}).error().code, "service_not_allowed");
}

TEST_F(RemoteServiceEndpointTest, SubscribingReturnsTheSnapshotAndPublishesUpdates) {
    m_service->m_state.change(ServiceContext{}, [](Json& draft) { draft["n"] = 5; });
    const auto snapshot = invoke(m_wire.subscribeCall("s1", "pi.counter", "singleton"));
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ((**snapshot)["instances"][0]["members"][1]["ops"][0][1]["n"], 5);
    EXPECT_TRUE(m_published.empty());
    ASSERT_TRUE(invoke(Json{{"serviceId", "pi.counter"}, {"member", "bump"}, {"args", Json::array()}}).has_value());
    ASSERT_EQ(m_published.size(), 1U);
    EXPECT_EQ(m_published[0].first, "s1");
    EXPECT_EQ(m_published[0].second["sequence"], 2);
    EXPECT_EQ(m_published[0].second["ops"], Json::parse(R"([["s",["n"],6]])"));
}

TEST_F(RemoteServiceEndpointTest, DuplicateAndUnknownSubscriptionsAreRejected) {
    ASSERT_TRUE(invoke(m_wire.subscribeCall("s1", "pi.counter", "singleton")).has_value());
    const auto duplicate = invoke(m_wire.subscribeCall("s1", "pi.counter", "singleton"));
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().message, "Service subscription ID is already active");
    EXPECT_EQ(invoke(m_wire.unsubscribeCall("other")).error().message, "Service subscription was not found");
    EXPECT_EQ(invoke(m_wire.subscribeCall("s2", "pi.nope", "singleton")).error().code, "service_not_allowed");
}

TEST_F(RemoteServiceEndpointTest, UnsubscribingStopsUpdates) {
    ASSERT_TRUE(invoke(m_wire.subscribeCall("s1", "pi.counter", "singleton")).has_value());
    const auto removed = invoke(m_wire.unsubscribeCall("s1"));
    ASSERT_TRUE(removed.has_value());
    EXPECT_FALSE(removed->has_value());
    ASSERT_TRUE(invoke(Json{{"serviceId", "pi.counter"}, {"member", "bump"}, {"args", Json::array()}}).has_value());
    EXPECT_TRUE(m_published.empty());
    EXPECT_TRUE(invoke(m_wire.subscribeCall("s1", "pi.counter", "singleton")).has_value());
}

TEST_F(RemoteServiceEndpointTest, DisposeClosesSubscriptionsAndRefusesCalls) {
    ASSERT_TRUE(invoke(m_wire.subscribeCall("s1", "pi.counter", "singleton")).has_value());
    m_endpoint.dispose();
    m_endpoint.dispose();
    m_service->m_state.change(ServiceContext{}, [](Json& draft) { draft["n"] = 9; });
    EXPECT_TRUE(m_published.empty());
    EXPECT_EQ(invoke(m_wire.catalogueCall()).error().code, "closed");
}
