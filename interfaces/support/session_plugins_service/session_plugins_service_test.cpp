#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.session_plugins_service;

TEST(SessionPluginsServiceTest, ReloadRunsTheHostsCallbackOneAtATime) {
    int reloads = 0;
    SessionPluginsService service([&]() -> Result<void> {
        ++reloads;
        return {};
    });
    const ServiceContext context{std::make_shared<AbortSignal>()};
    const auto result = service.methods().at("reload")({}, context);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
    EXPECT_EQ(reloads, 1);
}

TEST(SessionPluginsServiceTest, ReloadFailuresReachTheCaller) {
    SessionPluginsService service([]() -> Result<void> { return std::unexpected(Error{"plugin", "load failed"}); });
    const ServiceContext context{std::make_shared<AbortSignal>()};
    const auto result = service.methods().at("reload")({}, context);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "load failed");
}

TEST(SessionPluginsServiceTest, ReloadTakesNoArguments) {
    SessionPluginsService service([]() -> Result<void> { return {}; });
    const ServiceContext context{std::make_shared<AbortSignal>()};
    EXPECT_FALSE(service.methods().at("reload")({Json(1)}, context).has_value());
}
