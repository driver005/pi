#include <gtest/gtest.h>

import std;
import pi.support.package_patterns;

class PackagePatternsTest : public testing::Test {
protected:
    std::vector<std::string> m_all{"/pkg/skills/a/SKILL.md", "/pkg/skills/b/SKILL.md", "/pkg/prompts/review.md", "/pkg/prompts/ship.md"};
    PackagePatterns m_patterns;
};

TEST_F(PackagePatternsTest, NoPatternsKeepEverything) {
    EXPECT_EQ(m_patterns.apply(m_all, {}, "/pkg"), m_all);
}

TEST_F(PackagePatternsTest, IncludesNarrowAndExcludesRemove) {
    const auto included = m_patterns.apply(m_all, {"prompts/*.md"}, "/pkg");
    EXPECT_EQ(included, (std::vector<std::string>{"/pkg/prompts/review.md", "/pkg/prompts/ship.md"}));
    const auto excluded = m_patterns.apply(m_all, {"!prompts/ship.md"}, "/pkg");
    EXPECT_EQ(excluded.size(), 3u);
    EXPECT_EQ(std::ranges::count(excluded, "/pkg/prompts/ship.md"), 0);
}

TEST_F(PackagePatternsTest, SkillFilesMatchThroughTheirDirectory) {
    EXPECT_EQ(m_patterns.apply(m_all, {"skills/a"}, "/pkg"), (std::vector<std::string>{"/pkg/skills/a/SKILL.md"}));
    EXPECT_EQ(m_patterns.apply(m_all, {"b"}, "/pkg"), (std::vector<std::string>{"/pkg/skills/b/SKILL.md"}));
}

TEST_F(PackagePatternsTest, ForceIncludeBeatsExcludeAndForceExcludeBeatsAll) {
    const auto result = m_patterns.apply(m_all, {"!prompts/*", "+prompts/ship.md", "-skills/a"}, "/pkg");
    EXPECT_EQ(result, (std::vector<std::string>{"/pkg/skills/b/SKILL.md", "/pkg/prompts/ship.md"}));
}

TEST_F(PackagePatternsTest, DeltaNamesOnlyTheTouchedPaths) {
    const auto delta = m_patterns.delta(m_all, {"!prompts/*", "+prompts/ship.md", "-skills/b"}, "/pkg");
    ASSERT_EQ(delta.size(), 3u);
    EXPECT_EQ(delta[0], (std::pair<std::string, bool>{"/pkg/prompts/review.md", false}));
    EXPECT_EQ(delta[1], (std::pair<std::string, bool>{"/pkg/prompts/ship.md", true}));
    EXPECT_EQ(delta[2], (std::pair<std::string, bool>{"/pkg/skills/b/SKILL.md", false}));
}
