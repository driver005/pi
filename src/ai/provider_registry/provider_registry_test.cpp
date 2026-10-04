#include <gtest/gtest.h>

import std;
import pi.ai.faux_provider;
import pi.ai.provider_registry;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;

TEST(ProviderRegistryTest, RegisterFindReplaceUnregister) {
    InlineExecutor executor;
    FixedClock clock;
    ProviderRegistry registry;
    EXPECT_EQ(registry.find("faux"), nullptr);
    auto first = std::make_shared<FauxProvider>(executor, clock, "faux");
    registry.registerProvider(first);
    EXPECT_EQ(registry.find("faux"), first);
    auto second = std::make_shared<FauxProvider>(executor, clock, "faux");
    registry.registerProvider(second);
    EXPECT_EQ(registry.find("faux"), second);
    registry.registerProvider(std::make_shared<FauxProvider>(executor, clock, "other"));
    EXPECT_EQ(registry.apis(), (std::vector<std::string>{"faux", "other"}));
    registry.unregisterProvider("faux");
    EXPECT_EQ(registry.find("faux"), nullptr);
}
