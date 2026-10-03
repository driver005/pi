#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.presentation_plugins_service;
import pi.testing.fake_session_catalog;

class PresentationPluginsServiceTest : public testing::Test {
protected:
    Result<std::optional<Json>> call(const std::string& method, const std::vector<Json>& args) {
        return m_service.methods().at(method)(args, ServiceContext{std::make_shared<AbortSignal>()});
    }

    FakeSessionCatalog m_catalog;
    std::mutex m_mutations;
    PresentationPluginsService m_service{m_catalog, m_mutations};
};

TEST_F(PresentationPluginsServiceTest, PreparingASessionYieldsTheEmptySelection) {
    ASSERT_TRUE(m_catalog.create(std::string("demo")).has_value());
    const auto prepared = call("prepareSession", {Json{{"sessionId", "demo"}, {"packagePaths", nullptr}}});
    ASSERT_TRUE(prepared.has_value()) << prepared.error().message;
    EXPECT_EQ(**prepared, Json::parse(R"({"presentationFacetBundles":[]})"));
}

TEST_F(PresentationPluginsServiceTest, AnUnknownSessionIsRefused) {
    const auto prepared = call("prepareSession", {Json{{"sessionId", "nope"}, {"packagePaths", Json::array()}}});
    ASSERT_FALSE(prepared.has_value());
    EXPECT_EQ(prepared.error().code, "session_not_found");
}

TEST_F(PresentationPluginsServiceTest, MalformedRequestsAreRefused) {
    EXPECT_FALSE(call("prepareSession", {}).has_value());
    EXPECT_FALSE(call("prepareSession", {Json{{"packagePaths", nullptr}}}).has_value());
    ASSERT_TRUE(m_catalog.create(std::string("demo")).has_value());
    EXPECT_FALSE(call("prepareSession", {Json{{"sessionId", "demo"}, {"packagePaths", "x"}}}).has_value());
    EXPECT_FALSE(call("prepareSession", {Json{{"sessionId", "demo"}, {"packagePaths", Json::array({1})}}}).has_value());
    EXPECT_FALSE(call("reload", {Json(1)}).has_value());
}

TEST_F(PresentationPluginsServiceTest, ReloadNeedsAPreparedSelectionWhichDetachingClears) {
    ASSERT_TRUE(m_catalog.create(std::string("demo")).has_value());
    const auto early = call("reload", {});
    ASSERT_FALSE(early.has_value());
    EXPECT_EQ(early.error().code, "invalid_state");
    ASSERT_TRUE(call("prepareSession", {Json{{"sessionId", "demo"}, {"packagePaths", nullptr}}}).has_value());
    const auto reloaded = call("reload", {});
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(**reloaded, Json::parse(R"({"presentationFacetBundles":[]})"));
    m_service.clearPrepared();
    EXPECT_FALSE(call("reload", {}).has_value());
}
