#include <gtest/gtest.h>

import std;
import pi.support.short_id_generator;

TEST(ShortIdGeneratorTest, ProducesEightHexCharacters) {
    ShortIdGenerator generator;
    const std::string id = generator.next([](const std::string&) { return false; });
    EXPECT_EQ(id.size(), 8U);
    EXPECT_EQ(id.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(ShortIdGeneratorTest, SkipsTakenIds) {
    ShortIdGenerator generator(42);
    ShortIdGenerator twin(42);
    const std::string first = twin.next([](const std::string&) { return false; });
    const std::string second = generator.next([&](const std::string& id) { return id == first; });
    EXPECT_NE(second, first);
}

TEST(ShortIdGeneratorTest, FallsBackToLongIdWhenEverythingCollides) {
    ShortIdGenerator generator;
    const std::string id = generator.next([](const std::string& candidate) { return candidate.size() == 8; });
    EXPECT_EQ(id.size(), 36U);
}
