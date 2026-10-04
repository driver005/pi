#include <gtest/gtest.h>

import std;
import pi.support.semver_comparator;

TEST(SemverComparatorTest, OrdersVersionsNumerically) {
    const SemverComparator semver;
    EXPECT_EQ(semver.compare("1.2.3", "1.2.3"), 0);
    EXPECT_LT(*semver.compare("1.2.3", "1.10.0"), 0);
    EXPECT_GT(*semver.compare("2.0.0", "1.99.99"), 0);
    EXPECT_LT(*semver.compare("0.9.9", "0.10.0"), 0);
    EXPECT_EQ(semver.compare("v1.2.3", "1.2.3+build.5"), 0) << "a leading v and build metadata do not count";
}

TEST(SemverComparatorTest, PrereleasesFollowTheSemverRules) {
    const SemverComparator semver;
    // The precedence example of semver.org.
    const std::vector<std::string> ordered{"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"};
    for (std::size_t i = 0; i + 1 < ordered.size(); ++i) {
        EXPECT_LT(*semver.compare(ordered[i], ordered[i + 1]), 0) << ordered[i] << " < " << ordered[i + 1];
        EXPECT_GT(*semver.compare(ordered[i + 1], ordered[i]), 0);
    }
}

TEST(SemverComparatorTest, TextThatIsNotAVersionHasNoOrder) {
    const SemverComparator semver;
    for (const std::string text : {"", "1", "1.2", "1.2.3.4", "latest", "^1.2.3", "1.x.3", "1.2.3-", "1.2.3-a..b", "1.2.-3"}) {
        EXPECT_FALSE(semver.compare(text, "1.0.0").has_value()) << text;
        EXPECT_FALSE(semver.isExact(text)) << text;
    }
    EXPECT_TRUE(semver.isExact("1.2.3"));
    EXPECT_TRUE(semver.isExact("1.2.3-beta.1"));
}
