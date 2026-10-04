#include <gtest/gtest.h>

import std;
import pi.support.radius_gateway;
import pi.testing.fake_environment;

TEST(RadiusGatewayTest, NormalizesAndHonorsTheEnvironmentOverride) {
    RadiusGateway gateway;
    FakeEnvironment environment;
    EXPECT_EQ(gateway.gatewayUrl(environment), "https://radius.pi.dev");
    environment.set("PI_RADIUS_GATEWAY", "gw.example.com/");
    EXPECT_EQ(gateway.gatewayUrl(environment), "https://gw.example.com");
    environment.set("PI_RADIUS_GATEWAY", "HTTP://localhost:8080//");
    EXPECT_EQ(gateway.gatewayUrl(environment), "HTTP://localhost:8080");
    EXPECT_EQ(gateway.mcpUrl(), "https://radius.pi.dev/mcp");
}
