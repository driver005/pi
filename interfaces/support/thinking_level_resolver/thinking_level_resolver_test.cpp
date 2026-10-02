#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.thinking_level_resolver;

class ThinkingLevelResolverTest : public testing::Test {
protected:
    Model reasoningModel(const std::string& map) {
        Model model;
        model.reasoning = true;
        model.thinkingLevelMap = Json::parse(map);
        return model;
    }

    ThinkingLevelResolver m_resolver;
};

TEST_F(ThinkingLevelResolverTest, NonReasoningModelOnlyOff) {
    Model model;
    const auto levels = m_resolver.supportedLevels(model);
    ASSERT_EQ(levels.size(), 1U);
    EXPECT_EQ(levels[0], ThinkingLevel::Off);
}

TEST_F(ThinkingLevelResolverTest, ExtendedLevelsNeedMapEntry) {
    const auto plain = m_resolver.supportedLevels(reasoningModel("{}"));
    EXPECT_EQ(plain.size(), 5U);
    const auto extended = m_resolver.supportedLevels(reasoningModel(R"({"xhigh":"max"})"));
    EXPECT_EQ(extended.size(), 6U);
}

TEST_F(ThinkingLevelResolverTest, NullHidesLevel) {
    const auto levels = m_resolver.supportedLevels(reasoningModel(R"({"off":null,"minimal":null})"));
    EXPECT_EQ(levels.front(), ThinkingLevel::Low);
}

TEST_F(ThinkingLevelResolverTest, ClampPrefersHigher) {
    const Model model = reasoningModel(R"({"minimal":null})");
    EXPECT_EQ(m_resolver.clamp(model, ThinkingLevel::Minimal), ThinkingLevel::Low);
    EXPECT_EQ(m_resolver.clamp(model, ThinkingLevel::XHigh), ThinkingLevel::High);
    EXPECT_EQ(m_resolver.clamp(model, ThinkingLevel::Medium), ThinkingLevel::Medium);
}

TEST_F(ThinkingLevelResolverTest, ProviderEffortUsesMap) {
    const Model model = reasoningModel(R"({"xhigh":"max","low":"lite"})");
    EXPECT_EQ(m_resolver.providerEffort(model, ThinkingLevel::XHigh), "max");
    EXPECT_EQ(m_resolver.providerEffort(model, ThinkingLevel::Low), "lite");
    EXPECT_EQ(m_resolver.providerEffort(model, ThinkingLevel::High), "high");
}
