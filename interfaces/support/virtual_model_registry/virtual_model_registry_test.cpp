#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.virtual_model_registry;

class VirtualModelRegistryTest : public testing::Test {
protected:
    VirtualModelRegistryTest() {
        m_catalog["fast"] = physical("anthropic", "fast", false);
        m_catalog["deep"] = physical("anthropic", "deep", true);
        m_catalog["other"] = physical("openai", "other", false);
    }

    Model physical(const std::string& provider, const std::string& id, bool reasoning) {
        Model model;
        model.provider = provider;
        model.id = id;
        model.api = "faux";
        model.reasoning = reasoning;
        return model;
    }

    VirtualModelDefinition definition(const std::string& id, std::function<Result<VirtualRoute>(const VirtualRouteRequest&)> route) {
        VirtualModelDefinition out;
        out.provider = "router";
        out.id = id;
        out.name = "Auto";
        out.route = std::move(route);
        return out;
    }

    Result<void> add(const VirtualModelDefinition& definition) {
        return m_registry.add(definition, [this](const std::string& provider, const std::string& id) { return physicalLookup(provider, id).has_value(); });
    }

    std::optional<Model> physicalLookup(const std::string& provider, const std::string& id) {
        for (const auto& [key, model] : m_catalog) {
            if (model.provider == provider && model.id == id) {
                return model;
            }
        }
        return std::nullopt;
    }

    Result<VirtualRoute> resolve(const VirtualResolveRequest& request) {
        return m_registry.resolve(
            request, [this](const std::string& provider, const std::string& id) { return physicalLookup(provider, id); },
            [this](const std::string& provider) { return !m_unconfigured.contains(provider); });
    }

    VirtualResolveRequest requestFor(const std::string& id) {
        VirtualResolveRequest request;
        request.model = *m_registry.find("router", id);
        request.thinkingLevel = ThinkingLevel::High;
        return request;
    }

    VirtualModelRegistry m_registry;
    std::map<std::string, Model> m_catalog;
    std::set<std::string> m_unconfigured;
};

TEST_F(VirtualModelRegistryTest, ACatalogEntryOffersTheRequestedLevelsAndNeverReachesAProvider) {
    VirtualModelDefinition entry = definition("auto", [](const VirtualRouteRequest&) -> Result<VirtualRoute> { return VirtualRoute{}; });
    entry.thinkingLevels = {ThinkingLevel::Low, ThinkingLevel::High};
    entry.contextWindow = 4096;
    ASSERT_TRUE(add(entry).has_value());
    const auto model = m_registry.find("router", "auto");
    ASSERT_TRUE(model.has_value());
    EXPECT_TRUE(m_registry.isVirtual(*model));
    EXPECT_EQ(model->api, "pi-virtual");
    EXPECT_TRUE(model->reasoning);
    EXPECT_EQ(model->contextWindow, 4096);
    EXPECT_EQ(model->thinkingLevelMap["low"], "low");
    EXPECT_TRUE(model->thinkingLevelMap["medium"].is_null());
    EXPECT_EQ(model->input, (std::vector<std::string>{"text", "image"}));
    EXPECT_TRUE(m_registry.listsProvider("router"));
    EXPECT_EQ(m_registry.providers(), std::set<std::string>{"router"});

    ASSERT_TRUE(add(definition("plain", [](const VirtualRouteRequest&) -> Result<VirtualRoute> { return VirtualRoute{}; })).has_value());
    EXPECT_FALSE(m_registry.find("router", "plain")->reasoning);
    EXPECT_TRUE(m_registry.remove("router", "plain"));
    EXPECT_FALSE(m_registry.remove("router", "plain"));
    EXPECT_EQ(m_registry.models().size(), 1U);
}

TEST_F(VirtualModelRegistryTest, RegistrationRejectsEmptyNamesMissingRoutesAndPhysicalIds) {
    EXPECT_FALSE(add(definition(" ", [](const VirtualRouteRequest&) -> Result<VirtualRoute> { return VirtualRoute{}; })).has_value());
    EXPECT_FALSE(add(definition("auto", nullptr)).has_value());
    VirtualModelDefinition clash = definition("fast", [](const VirtualRouteRequest&) -> Result<VirtualRoute> { return VirtualRoute{}; });
    clash.provider = "anthropic";
    const auto rejected = add(clash);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().message, "Virtual model anthropic/fast conflicts with a physical model.");
}

TEST_F(VirtualModelRegistryTest, RoutingChecksTheAnswerAndClampsTheLevel) {
    VirtualRouteRequest seen;
    ASSERT_TRUE(add(definition("auto", [&](const VirtualRouteRequest& request) -> Result<VirtualRoute> {
        seen = request;
        VirtualRoute route;
        route.model = *physicalLookup("anthropic", "fast");
        route.thinkingLevel = ThinkingLevel::High;
        route.state = Json{{"phase", "build"}};
        return route;
    })).has_value());
    VirtualResolveRequest request = requestFor("auto");
    request.reason = "continuation";
    request.state = Json{{"phase", "plan"}};
    AssistantMessage answered;
    answered.provider = "anthropic";
    answered.model = "deep";
    answered.thinkingLevel = ThinkingLevel::Medium;
    answered.stopReason = StopReason::Stop;
    AssistantMessage failed;
    failed.provider = "openai";
    failed.model = "other";
    failed.stopReason = StopReason::Error;
    request.messages = {Message(answered), Message(failed)};
    const auto route = resolve(request);
    ASSERT_TRUE(route.has_value()) << route.error().message;
    EXPECT_EQ(route->model.id, "fast");
    EXPECT_EQ(route->thinkingLevel, ThinkingLevel::Off);
    ASSERT_TRUE(route->state.has_value());
    EXPECT_EQ((*route->state)["phase"], "build");
    EXPECT_EQ(seen.reason, "continuation");
    EXPECT_EQ(seen.state["phase"], "plan");
    EXPECT_EQ(seen.thinkingLevel, ThinkingLevel::High);
    ASSERT_TRUE(seen.previous.has_value());
    EXPECT_EQ(seen.previous->model.id, "deep");
    EXPECT_EQ(seen.previous->thinkingLevel, std::optional<ThinkingLevel>(ThinkingLevel::Medium));
    EXPECT_FALSE(seen.failed.has_value());
}

TEST_F(VirtualModelRegistryTest, ARetrySeesTheFailedRequest) {
    VirtualRouteRequest seen;
    ASSERT_TRUE(add(definition("auto", [&](const VirtualRouteRequest& request) -> Result<VirtualRoute> {
        seen = request;
        VirtualRoute route;
        route.model = request.failed ? request.failed->model : *physicalLookup("openai", "other");
        route.thinkingLevel = ThinkingLevel::High;
        return route;
    })).has_value());
    VirtualResolveRequest request = requestFor("auto");
    request.reason = "retry";
    AssistantMessage failed;
    failed.provider = "anthropic";
    failed.model = "deep";
    failed.stopReason = StopReason::Error;
    failed.errorMessage = "overloaded";
    request.failed = failed;
    const auto route = resolve(request);
    ASSERT_TRUE(route.has_value()) << route.error().message;
    EXPECT_EQ(route->model.id, "deep");
    EXPECT_EQ(route->thinkingLevel, ThinkingLevel::High);
    ASSERT_TRUE(seen.failed.has_value());
    EXPECT_EQ(seen.failed->message.errorMessage, std::optional<std::string>("overloaded"));
}

TEST_F(VirtualModelRegistryTest, AnswersThatCannotBeUsedFailTheRequest) {
    std::string target = "missing";
    ASSERT_TRUE(add(definition("auto", [&](const VirtualRouteRequest&) -> Result<VirtualRoute> {
        if (target == "boom") {
            return std::unexpected(Error{"x", "no luck"});
        }
        VirtualRoute route;
        route.model.provider = "anthropic";
        route.model.id = target;
        return route;
    })).has_value());
    const VirtualResolveRequest request = requestFor("auto");
    auto missing = resolve(request);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message, "Virtual model router/auto routed to anthropic/missing, which is not a physical model.");
    target = "fast";
    m_unconfigured.insert("anthropic");
    auto unconfigured = resolve(request);
    ASSERT_FALSE(unconfigured.has_value());
    EXPECT_EQ(unconfigured.error().message, "Virtual model router/auto routed to anthropic/fast, which has no credentials.");
    target = "boom";
    auto failed = resolve(request);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "Virtual model router/auto could not route: no luck");

    VirtualResolveRequest unknown = request;
    unknown.model.id = "gone";
    EXPECT_EQ(resolve(unknown).error().message, "Virtual model router/gone is not registered.");
}
