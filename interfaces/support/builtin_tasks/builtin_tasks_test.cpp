#include <gtest/gtest.h>

import std;
import pi.support.builtin_tasks;
import pi.support.registry;

TEST(BuiltinTasksTest, DefinitionsMatchTheKindsAndSeedTheirFirstCheckpoints) {
    BuiltinTasks builtins;
    const auto definitions = builtins.all();
    ASSERT_EQ(definitions.size(), 3u);
    std::vector<std::string> names;
    for (const auto& definition : definitions) {
        names.push_back(definition->name);
        EXPECT_EQ(definition->version, 1);
        EXPECT_TRUE(static_cast<bool>(definition->abort));
    }
    EXPECT_EQ(names, builtins.kinds());
    EXPECT_EQ(definitions[0]->initial(Json::object()).dump(), R"({"phase":"prepare","attempt":1})");
    EXPECT_EQ(definitions[1]->initial(Json::object()).dump(), R"({"phase":"call"})");
    EXPECT_EQ(definitions[2]->initial(Json::object()).dump(), R"({"phase":"select"})");
    EXPECT_EQ(definitions[0]->phases.size(), 5u);
}

TEST(BuiltinTasksTest, AreResolvableFromARegistryBuiltWithThem) {
    BuiltinTasks builtins;
    Registry registry(builtins.all());
    for (const std::string& kind : builtins.kinds()) {
        EXPECT_TRUE(registry.snapshot()->task(kind) != nullptr) << kind;
    }
}
