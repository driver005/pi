#include "src/base/system_environment/system_environment.h"

#include <gtest/gtest.h>

TEST(SystemEnvironmentTest, SetGetUnset) {
    SystemEnvironment env;
    EXPECT_FALSE(env.get("PI_TEST_ENV_VAR").has_value());
    env.set("PI_TEST_ENV_VAR", "value");
    EXPECT_EQ(env.get("PI_TEST_ENV_VAR"), std::optional<std::string>("value"));
    EXPECT_EQ(env.all().at("PI_TEST_ENV_VAR"), "value");
    env.unset("PI_TEST_ENV_VAR");
    EXPECT_FALSE(env.get("PI_TEST_ENV_VAR").has_value());
}
