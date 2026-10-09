#include <gtest/gtest.h>

import std;
import pi.support.theme_color_parser;

class ThemeColorParserTest : public testing::Test {
protected:
    std::string hexOf(const Json& value) {
        const auto color = m_parser.parse(value);
        EXPECT_TRUE(color.has_value()) << value.dump();
        return color ? m_parser.toHex(color->rgb) : std::string();
    }

    ThemeColorParser m_parser;
};

TEST_F(ThemeColorParserTest, ParsesHexInBothLengths) {
    EXPECT_EQ(hexOf("#1a2B3c"), "#1a2b3c");
    EXPECT_EQ(hexOf("#fa0"), "#ffaa00");
}

TEST_F(ThemeColorParserTest, ParsesThePalette) {
    EXPECT_EQ(hexOf(0), "#000000");
    EXPECT_EQ(hexOf(9), "#ff0000");
    EXPECT_EQ(hexOf(16), "#000000");
    EXPECT_EQ(hexOf(196), "#ff0000");
    EXPECT_EQ(hexOf(231), "#ffffff");
    EXPECT_EQ(hexOf(232), "#080808");
    EXPECT_EQ(hexOf(255), "#eeeeee");
    EXPECT_TRUE(m_parser.parse(Json(7))->terminalPalette);
    EXPECT_FALSE(m_parser.parse(Json(100))->terminalPalette);
}

TEST_F(ThemeColorParserTest, ParsesOklchAndOkhslWithPercentages) {
    EXPECT_EQ(hexOf("oklch(100% 0 0)"), "#ffffff");
    EXPECT_EQ(hexOf("oklch(0 0 0)"), "#000000");
    EXPECT_EQ(hexOf("OKLCH(0.5 0 90deg)"), hexOf("oklch(50% 0 0)"));
    EXPECT_EQ(hexOf("okhsl(120 0 1)"), "#ffffff");
    EXPECT_EQ(hexOf("okhsl(120 0% 100%)"), "#ffffff");
    EXPECT_NEAR(m_parser.parse(Json("oklch(40% 0.1 200)"))->lightness, 0.4, 1e-12);
}

TEST_F(ThemeColorParserTest, AcceptsSignsExponentsAndMixedCase) {
    EXPECT_EQ(hexOf("oklch(+1e0 0 0)"), "#ffffff");
    EXPECT_EQ(hexOf("OkHsL(+120DEG .0 1.)"), "#ffffff");
    EXPECT_EQ(hexOf("oklch(  50%   0   0deg )"), hexOf("oklch(0.5 0 0)"));
    EXPECT_TRUE(m_parser.isOkColorFunction("OKLCH(1 0 0)"));
    EXPECT_TRUE(m_parser.isOkhsl("okhsl(1 0 0)"));
    EXPECT_FALSE(m_parser.isOkhsl("oklch(1 0 0)"));
}

TEST_F(ThemeColorParserTest, RejectsInvalidValues) {
    for (const Json& value :
         {Json("red"), Json("#12"), Json("oklch(2 0 0)"), Json("oklch(0.5 -1 0)"),
          Json("okhsl(0 2 0.5)"), Json("oklch(1e999 0 0)"), Json("okhsl(1e999 0.5 0.5)"),
          Json("oklch(inf 0 0)"), Json("oklch(0.5 nan 0)"), Json("oklch(0.5 0.1)"),
          Json("oklch(0.5 0.1 20 30)"), Json("oklch(0.5 0.1 20deg"), Json("oklch(5%x 0.1 20)"),
          Json("okhsl(10 50% 50)x"), Json(256), Json(-1), Json(true), Json(1.5)}) {
        EXPECT_FALSE(m_parser.parse(value).has_value()) << value.dump();
    }
}
