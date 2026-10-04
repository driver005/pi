#include <gtest/gtest.h>

import std;
import pi.support.virtual_model_names;

TEST(VirtualModelNamesTest, VirtualModelsAreMarkedByTheirApi) {
    const VirtualModelNames names;
    Model virtualModel;
    virtualModel.api = names.api();
    Model physical;
    physical.api = "openai-completions";
    EXPECT_TRUE(names.isVirtual(virtualModel));
    EXPECT_FALSE(names.isVirtual(physical));
    EXPECT_TRUE(names.isVirtualApi("pi-virtual"));
    EXPECT_EQ(names.stateEntryType(), "pi.virtual-model-state");
}
