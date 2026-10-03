#include <gtest/gtest.h>

import std;
import pi.support.registry_state;

class RegistryStateTest : public ::testing::Test {
protected:
    std::shared_ptr<const TaskDefinition> task(const std::string& name) {
        auto definition = std::make_shared<TaskDefinition>();
        definition->name = name;
        return definition;
    }

    std::shared_ptr<const Extension> extension(const std::string& name, std::vector<std::shared_ptr<const TaskDefinition>> tasks) {
        auto result = std::make_shared<Extension>();
        result->name = name;
        result->tasks = std::move(tasks);
        return result;
    }
};

TEST_F(RegistryStateTest, ResolvesBuiltinsAndExtensionTasksByName) {
    RegistryState state;
    ASSERT_TRUE(state.assign({extension("a", {task("a.work")}), extension("b", {})}, {task("pi.generation")}).has_value());
    EXPECT_TRUE(state.task("pi.generation") != nullptr);
    EXPECT_TRUE(state.task("a.work") != nullptr);
    EXPECT_TRUE(state.task("missing") == nullptr);
    EXPECT_EQ(state.installed().size(), 2u);
    EXPECT_TRUE(state.extension("b") != nullptr);
    EXPECT_TRUE(state.extension("c") == nullptr);
    EXPECT_EQ(state.tasks().size(), 2u);
}

TEST_F(RegistryStateTest, TaskNameCollisionsFail) {
    RegistryState state;
    auto collision = state.assign({extension("a", {task("pi.generation")})}, {task("pi.generation")});
    ASSERT_FALSE(collision.has_value());
    EXPECT_NE(collision.error().message.find("already installed"), std::string::npos);
    EXPECT_FALSE(state.assign({extension("a", {task("x")}), extension("b", {task("x")})}, {}).has_value());
}
