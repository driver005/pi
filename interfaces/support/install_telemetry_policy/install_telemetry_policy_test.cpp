#include <gtest/gtest.h>

import std;
import pi.support.install_telemetry_policy;

TEST(InstallTelemetryPolicyTest, TheEnvironmentOverridesTheSetting) {
    InstallTelemetryPolicy policy;
    const SettingsView on(Json::object());
    const SettingsView off(Json{{"enableInstallTelemetry", false}});
    EXPECT_TRUE(policy.enabled(on, std::nullopt));
    EXPECT_FALSE(policy.enabled(off, std::nullopt));
    for (const std::string value : {"1", "true", "TRUE", "Yes"}) {
        EXPECT_TRUE(policy.enabled(off, value)) << value;
    }
    for (const std::string value : {"0", "false", "", "off", "2"}) {
        EXPECT_FALSE(policy.enabled(on, value)) << value;
    }
}
