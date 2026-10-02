#include "src/base/uuid7_generator/uuid7_generator.h"

#include <gtest/gtest.h>

#include <set>

class StepClock : public IClock {
public:
    std::int64_t nowMs() const override {
        return m_now;
    }
    std::int64_t m_now = 1'700'000'000'000;
};

TEST(Uuid7GeneratorTest, FormatAndVersionBits) {
    StepClock clock;
    Uuid7Generator generator(clock);
    const std::string id = generator.next();
    ASSERT_EQ(id.size(), 36U);
    EXPECT_EQ(id[8], '-');
    EXPECT_EQ(id[13], '-');
    EXPECT_EQ(id[14], '7');
    EXPECT_NE(std::string("89ab").find(id[19]), std::string::npos);
}

TEST(Uuid7GeneratorTest, StrictlyIncreasingWithinSameMillisecond) {
    StepClock clock;
    Uuid7Generator generator(clock);
    std::string previous = generator.next();
    std::set<std::string> seen{previous};
    for (int i = 0; i < 5000; ++i) {
        const std::string id = generator.next();
        EXPECT_GT(id, previous);
        EXPECT_TRUE(seen.insert(id).second);
        previous = id;
    }
}

TEST(Uuid7GeneratorTest, TimestampPrefixTracksClock) {
    StepClock clock;
    Uuid7Generator generator(clock);
    const std::string first = generator.next();
    clock.m_now += 1000;
    const std::string second = generator.next();
    EXPECT_GT(second, first);
}
