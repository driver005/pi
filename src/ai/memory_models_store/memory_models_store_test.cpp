#include <gtest/gtest.h>

import std;
import pi.ai.memory_models_store;

TEST(MemoryModelsStoreTest, WriteReadRemove) {
    MemoryModelsStore store;
    EXPECT_FALSE(store.read("p")->has_value());
    ModelsStoreEntry entry;
    entry.etag = "e";
    ASSERT_TRUE(store.write("p", entry));
    EXPECT_EQ((*store.read("p"))->etag, "e");
    ASSERT_TRUE(store.remove("p"));
    EXPECT_FALSE(store.read("p")->has_value());
}
