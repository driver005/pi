#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "pi_plugin.h"

import std;
import pi.support.plugin_tool;

TEST(PluginToolTest, ExposesItsDeclaration) {
    Tool declaration;
    declaration.name = "t";
    declaration.description = "d";
    PluginTool tool(declaration, "Label", "snippet", {"rule"}, ToolExecutionMode::Parallel,
                    [](void*, PiString, PiString, const PiAbort*, PiUpdateFn, void*) {
                        return PiOwnedString{nullptr, 0, nullptr};
                    },
                    nullptr);
    EXPECT_EQ(tool.definition().name, "t");
    EXPECT_EQ(tool.label(), "Label");
    EXPECT_EQ(tool.promptSnippet(), "snippet");
    EXPECT_EQ(tool.promptGuidelines(), std::vector<std::string>{"rule"});
    EXPECT_EQ(*tool.executionMode(), ToolExecutionMode::Parallel);
    EXPECT_EQ(tool.prepareArguments(Json{{"a", 1}}), (Json{{"a", 1}}));
}
