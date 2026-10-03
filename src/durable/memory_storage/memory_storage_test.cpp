#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.testing.storage_conformance;

class OffByOneStorage : public MemoryStorage {
public:
    Result<std::int64_t> mintId() override {
        auto minted = MemoryStorage::mintId();
        return minted ? Result<std::int64_t>(*minted + 1) : minted;
    }
};

TEST(MemoryStorageTest, PassesTheStorageConformanceSuite) {
    StorageConformance suite;
    const auto failures = suite.run([] { return std::unique_ptr<IStorage>(std::make_unique<MemoryStorage>()); });
    for (const std::string& failure : failures) {
        ADD_FAILURE() << failure;
    }
}

TEST(MemoryStorageTest, TheConformanceSuiteNoticesABrokenBackend) {
    StorageConformance suite;
    const auto failures = suite.run([] { return std::unique_ptr<IStorage>(std::make_unique<OffByOneStorage>()); });
    EXPECT_FALSE(failures.empty());
}
