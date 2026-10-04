#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.hook_bus;
import pi.support.plugin_resource_loader;
import pi.testing.fake_resource_loader;

class PluginResourceLoaderTest : public testing::Test {
protected:
    void on(const std::function<Json(const Json&)>& handler) {
        m_bus.subscribe("resources_discover", [handler](const std::string&, const Json& payload) -> Result<Json> { return handler(payload); });
    }

    HookBus m_bus;
    PluginSessionEvents m_events{m_bus};
    FakeResourceLoader m_inner;
    std::vector<std::vector<std::string>> m_skills;
    std::vector<std::vector<std::string>> m_prompts;
    PluginResourceLoader m_loader{m_inner, m_events, "/work", [this](const std::vector<std::string>& skills, const std::vector<std::string>& prompts) {
                                      m_skills.push_back(skills);
                                      m_prompts.push_back(prompts);
                                  }};
};

TEST_F(PluginResourceLoaderTest, WithoutSubscribersItJustLoads) {
    ASSERT_TRUE(m_loader.reload().has_value());
    EXPECT_TRUE(m_skills.empty());
}

TEST_F(PluginResourceLoaderTest, PluginPathsAreAddedOnceWithTheDiscoverReason) {
    std::vector<std::string> reasons;
    on([&reasons](const Json& payload) {
        reasons.push_back(payload["reason"].get<std::string>());
        EXPECT_EQ(payload["cwd"], "/work");
        return Json{{"skillPaths", Json::array({"/s/a", "/s/b"})}, {"promptPaths", Json::array({"/p/a"})}};
    });
    on([](const Json&) { return Json{{"skillPaths", Json::array({"/s/b", "/s/c", 7})}}; });
    ASSERT_TRUE(m_loader.reload().has_value());
    ASSERT_TRUE(m_loader.reload().has_value());
    EXPECT_EQ(reasons, (std::vector<std::string>{"startup", "reload"}));
    ASSERT_EQ(m_skills.size(), 1U) << "the second load has nothing new";
    EXPECT_EQ(m_skills[0], (std::vector<std::string>{"/s/a", "/s/b", "/s/c"}));
    EXPECT_EQ(m_prompts[0], (std::vector<std::string>{"/p/a"}));
}
