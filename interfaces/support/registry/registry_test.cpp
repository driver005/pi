#include <gtest/gtest.h>

import std;
import pi.support.registry;

class RegistryTest : public ::testing::Test {
protected:
    std::shared_ptr<const TaskDefinition> task(const std::string& name) {
        auto definition = std::make_shared<TaskDefinition>();
        definition->name = name;
        return definition;
    }

    Extension extension(const std::string& name) {
        Extension result;
        result.name = name;
        return result;
    }

    Registry m_registry{{std::make_shared<TaskDefinition>(TaskDefinition{"pi.generation", 1, nullptr, {}, nullptr, nullptr})}};
};

TEST_F(RegistryTest, InstallAppendsReplacesInPlaceAndUninstallAppendsAgain) {
    ASSERT_TRUE(m_registry.install(extension("a")).has_value());
    ASSERT_TRUE(m_registry.install(extension("b")).has_value());
    Extension replaced = extension("a");
    replaced.tools.push_back(ToolRegistration{"t", "d", Json::object(), "unsafe", std::nullopt, nullptr, std::nullopt, nullptr});
    ASSERT_TRUE(m_registry.install(replaced).has_value());
    auto installed = m_registry.snapshot()->installed();
    ASSERT_EQ(installed.size(), 2u);
    EXPECT_EQ(installed[0]->name, "a");
    EXPECT_EQ(installed[0]->tools.size(), 1u);
    ASSERT_TRUE(m_registry.uninstall("a").has_value());
    ASSERT_TRUE(m_registry.install(extension("a")).has_value());
    installed = m_registry.snapshot()->installed();
    EXPECT_EQ(installed[0]->name, "b");
    EXPECT_EQ(installed[1]->name, "a");
    EXPECT_TRUE(m_registry.uninstall("missing").has_value());
}

TEST_F(RegistryTest, SnapshotsAreImmutableAndListenersRunOnPublication) {
    int notified = 0;
    const std::int64_t handle = m_registry.subscribe([&] { ++notified; });
    auto before = m_registry.snapshot();
    ASSERT_TRUE(m_registry.install(extension("a")).has_value());
    EXPECT_EQ(before->installed().size(), 0u);
    EXPECT_EQ(m_registry.snapshot()->installed().size(), 1u);
    EXPECT_EQ(notified, 1);
    m_registry.unsubscribe(handle);
    ASSERT_TRUE(m_registry.uninstall("a").has_value());
    EXPECT_EQ(notified, 1);
}

TEST_F(RegistryTest, BuiltinTasksAreAlwaysThereAndCannotBeShadowed) {
    EXPECT_TRUE(m_registry.snapshot()->task("pi.generation") != nullptr);
    Extension shadow = extension("shadow");
    shadow.tasks.push_back(task("pi.generation"));
    auto result = m_registry.install(shadow);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("already installed"), std::string::npos);
    EXPECT_EQ(m_registry.snapshot()->installed().size(), 0u);
}

TEST_F(RegistryTest, ValidatesToolsAndSections) {
    Extension twoTools = extension("x");
    twoTools.tools.push_back(ToolRegistration{"t", "", Json::object(), "unsafe", std::nullopt, nullptr, std::nullopt, nullptr});
    twoTools.tools.push_back(ToolRegistration{"t", "", Json::object(), "unsafe", std::nullopt, nullptr, std::nullopt, nullptr});
    EXPECT_FALSE(m_registry.install(twoTools).has_value());
    for (const std::string& key : {"Bad", "1x", "has space", "instructions", ""}) {
        Extension bad = extension("y");
        bad.sections.push_back(PromptSection{key, nullptr, true});
        EXPECT_FALSE(m_registry.install(bad).has_value()) << key;
    }
    Extension duplicate = extension("z");
    duplicate.sections.push_back(PromptSection{"ok", nullptr, true});
    duplicate.sections.push_back(PromptSection{"ok", nullptr, true});
    EXPECT_FALSE(m_registry.install(duplicate).has_value());
    Extension fine = extension("w");
    fine.sections.push_back(PromptSection{"env-info_2", nullptr, true});
    EXPECT_TRUE(m_registry.install(fine).has_value());
}
