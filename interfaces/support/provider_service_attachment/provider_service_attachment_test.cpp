#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.provider_service_attachment;
import pi.support.remote_service_provider;
import pi.support.replicated_state;

class EchoService : public IRemoteService {
public:
    std::map<std::string, Method> methods() override {
        return {{"echo", [](const std::vector<Json>& args, const ServiceContext&) {
                     return Result<std::optional<Json>>(std::optional<Json>(Json(args)));
                 }}};
    }
    std::map<std::string, IReplicatedState*> states() override {
        return {{"state", &m_state}};
    }

    ReplicatedState m_state{Json{{"count", 0}}};
};

class ProviderServiceAttachmentTest : public ::testing::Test {
protected:
    ProviderServiceAttachmentTest() {
        m_provider = std::make_shared<RemoteServiceProvider>(std::vector<ServiceDefinition>{{"svc.echo", "singleton"}});
        m_service = std::make_shared<EchoService>();
        m_provider->provide("svc.echo", m_service);
        m_attachment = std::make_unique<ProviderServiceAttachment>(
            m_provider, [this] { ++m_disposed; }, [this] { ++m_released; });
    }

    ServiceContext context() const {
        return ServiceContext{std::make_shared<AbortSignal>()};
    }

    const IServiceEndpoint::Publisher m_publish = [](const std::string&, const Json&, const ServiceContext&) {};
    std::shared_ptr<RemoteServiceProvider> m_provider;
    std::shared_ptr<EchoService> m_service;
    std::unique_ptr<ProviderServiceAttachment> m_attachment;
    int m_disposed = 0;
    int m_released = 0;
};

TEST_F(ProviderServiceAttachmentTest, CallsReachTheProvider) {
    const Json call = {{"serviceId", "svc.echo"}, {"member", "echo"}, {"args", Json::array({1, "x"})}};
    const auto result = m_attachment->invokeService(call, m_publish, context());
    ASSERT_TRUE(result);
    EXPECT_EQ(**result, Json::array({1, "x"}));
}

TEST_F(ProviderServiceAttachmentTest, ControlCallsListTheCatalogue) {
    const Json call = {{"serviceId", "$chord.service"}, {"member", "catalogue"}, {"args", Json::array()}};
    const auto result = m_attachment->invokeService(call, m_publish, context());
    ASSERT_TRUE(result);
    ASSERT_EQ((*result)->size(), 1u);
    EXPECT_EQ((**result)[0]["serviceId"], "svc.echo");
}

TEST_F(ProviderServiceAttachmentTest, SubscriptionsDeliverStateUpdates) {
    std::vector<std::string> subscriptions;
    const IServiceEndpoint::Publisher publish = [&](const std::string& id, const Json&, const ServiceContext&) {
        subscriptions.push_back(id);
    };
    const Json subscribe = {{"serviceId", "$chord.service"},
                            {"member", "subscribe"},
                            {"args", Json::array({"sub1", "svc.echo", "singleton"})}};
    ASSERT_TRUE(m_attachment->invokeService(subscribe, publish, context()));
    m_service->m_state.change(context(), [](Json& draft) { draft["count"] = 1; });
    EXPECT_EQ(subscriptions, (std::vector<std::string>{"sub1"}));
}

TEST_F(ProviderServiceAttachmentTest, ReleaseEndsTheAttachmentOnce) {
    m_attachment->release(context());
    m_attachment->release(context());
    EXPECT_EQ(m_disposed, 1);
    EXPECT_EQ(m_released, 1);
    const Json call = {{"serviceId", "svc.echo"}, {"member", "echo"}, {"args", Json::array()}};
    EXPECT_EQ(m_attachment->invokeService(call, m_publish, context()).error().code, "invalid_state");
}

TEST_F(ProviderServiceAttachmentTest, ReleaseStopsSubscriptionUpdates) {
    std::vector<std::string> subscriptions;
    const IServiceEndpoint::Publisher publish = [&](const std::string& id, const Json&, const ServiceContext&) {
        subscriptions.push_back(id);
    };
    const Json subscribe = {{"serviceId", "$chord.service"},
                            {"member", "subscribe"},
                            {"args", Json::array({"sub1", "svc.echo", "singleton"})}};
    ASSERT_TRUE(m_attachment->invokeService(subscribe, publish, context()));
    m_attachment->release(context());
    m_service->m_state.change(context(), [](Json& draft) { draft["count"] = 2; });
    EXPECT_TRUE(subscriptions.empty());
}
