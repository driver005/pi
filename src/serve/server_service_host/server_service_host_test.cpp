#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.serve.server_service_host;
import pi.testing.fake_session_catalog;

class CountingPresentation : public IServerPresentation {
public:
    Result<void> attachSession(const std::string& sessionId, const ServiceContext&) override {
        m_attached.push_back(sessionId);
        return {};
    }
    Result<void> detachSession(const ServiceContext&) override {
        ++m_detached;
        return {};
    }
    Result<void> prepareSessionRemoval(const std::string&, const ServiceContext&) override {
        return {};
    }

    std::vector<std::string> m_attached;
    int m_detached = 0;
};

class ServerServiceHostTest : public ::testing::Test {
protected:
    ServiceContext context() const {
        return ServiceContext{std::make_shared<AbortSignal>()};
    }

    Json invoke(IServiceAttachment& attachment, const std::string& service, const std::string& member,
                const Json& args = Json::array(), const IServiceEndpoint::Publisher& publish = {}) {
        const Json call = {{"serviceId", service}, {"member", member}, {"args", args}};
        auto result = attachment.invokeService(
            call, publish ? publish : [](const std::string&, const Json&, const ServiceContext&) {}, context());
        EXPECT_TRUE(result) << (result ? "" : result.error().message);
        return result && result->has_value() ? **result : Json();
    }

    FakeSessionCatalog m_catalog;
    ServerServiceHost m_host{m_catalog, "server"};
    CountingPresentation m_presentation;
};

TEST_F(ServerServiceHostTest, OffersTheDirectoryAndManagementServices) {
    auto attachment = m_host.attachClient(m_presentation, context());
    ASSERT_TRUE(attachment);
    const Json catalogue = invoke(**attachment, "$chord.service", "catalogue");
    std::set<std::string> ids;
    for (const Json& entry : catalogue) {
        ids.insert(entry["serviceId"].get<std::string>());
    }
    EXPECT_EQ(ids, (std::set<std::string>{"pi.session-directory", "pi.session-management", "pi.presentation-plugins"}));
}

TEST_F(ServerServiceHostTest, ManagementCallsReachThePresentationAndTheCatalog) {
    auto attachment = m_host.attachClient(m_presentation, context());
    ASSERT_TRUE(attachment);
    const Json created = invoke(**attachment, "pi.session-management", "create", Json::array({Json::object()}));
    EXPECT_EQ(created["sessionId"], "s1");
    invoke(**attachment, "pi.session-management", "attach", Json::array({"s1"}));
    invoke(**attachment, "pi.session-management", "detach");
    EXPECT_EQ(m_presentation.m_attached, (std::vector<std::string>{"s1"}));
    EXPECT_EQ(m_presentation.m_detached, 1);
}

TEST_F(ServerServiceHostTest, EveryClientSeesDirectoryChanges) {
    auto first = m_host.attachClient(m_presentation, context());
    CountingPresentation otherPresentation;
    auto second = m_host.attachClient(otherPresentation, context());
    ASSERT_TRUE(first && second);
    std::vector<std::string> seen;
    const IServiceEndpoint::Publisher publish = [&](const std::string& id, const Json&, const ServiceContext&) {
        seen.push_back(id);
    };
    const Json snapshot = invoke(**second, "$chord.service", "subscribe",
                                 Json::array({"dir", "pi.session-directory", "singleton"}), publish);
    EXPECT_EQ(snapshot["instances"][0]["members"][0]["ops"][0][1]["sessions"].size(), 0u);
    invoke(**first, "pi.session-management", "create", Json::array({Json{{"id", "shared"}}}));
    EXPECT_EQ(seen, (std::vector<std::string>{"dir"}));
}

TEST_F(ServerServiceHostTest, RefreshPicksUpOutOfBandChanges) {
    auto attachment = m_host.attachClient(m_presentation, context());
    ASSERT_TRUE(attachment);
    std::vector<std::string> seen;
    const IServiceEndpoint::Publisher publish = [&](const std::string& id, const Json&, const ServiceContext&) {
        seen.push_back(id);
    };
    invoke(**attachment, "$chord.service", "subscribe", Json::array({"dir", "pi.session-directory", "singleton"}), publish);
    m_catalog.create(std::string("external"));
    ASSERT_TRUE(m_host.refresh(context()));
    EXPECT_EQ(seen.size(), 1u);
}

TEST_F(ServerServiceHostTest, ReleasedAttachmentsStopWorking) {
    auto attachment = m_host.attachClient(m_presentation, context());
    ASSERT_TRUE(attachment);
    (*attachment)->release(context());
    const Json call = {{"serviceId", "pi.session-management"}, {"member", "detach"}, {"args", Json::array()}};
    EXPECT_FALSE((*attachment)->invokeService(call, [](const std::string&, const Json&, const ServiceContext&) {}, context()));
}

TEST_F(ServerServiceHostTest, PresentationPluginsFollowTheClientsAttachment) {
    ASSERT_TRUE(m_catalog.create(std::string("demo")).has_value());
    auto attachment = m_host.attachClient(m_presentation, context());
    ASSERT_TRUE(attachment);
    const Json selection = invoke(**attachment, "pi.presentation-plugins", "prepareSession", Json::array({Json{{"sessionId", "demo"}, {"packagePaths", nullptr}}}));
    EXPECT_EQ(selection, Json::parse(R"({"presentationFacetBundles":[]})"));
    EXPECT_EQ(invoke(**attachment, "pi.presentation-plugins", "reload"), Json::parse(R"({"presentationFacetBundles":[]})"));
    invoke(**attachment, "pi.session-management", "detach");
    const Json call = {{"serviceId", "pi.presentation-plugins"}, {"member", "reload"}, {"args", Json::array()}};
    const auto after = (*attachment)->invokeService(call, [](const std::string&, const Json&, const ServiceContext&) {}, context());
    EXPECT_FALSE(after.has_value());
}
