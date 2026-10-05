#include <gtest/gtest.h>

import std;
import pi.support.oklab_converter;

class OklabConverterTest : public testing::Test {
protected:
    void expectRgb(const RgbColor& actual, double r, double g, double b) {
        EXPECT_EQ(actual.r, r);
        EXPECT_EQ(actual.g, g);
        EXPECT_EQ(actual.b, b);
    }

    OklabConverter m_converter;
};

TEST_F(OklabConverterTest, RoundTripsSrgbThroughOklab) {
    for (const RgbColor& color : {RgbColor{0, 0, 0}, RgbColor{255, 255, 255}, RgbColor{52, 53, 65}, RgbColor{200, 30, 90}}) {
        const OklabConverter::Vector lab = m_converter.rgbToOklab(color);
        expectRgb(m_converter.linearSrgbToRgb(m_converter.oklabToLinearSrgb(lab)), color.r, color.g, color.b);
    }
    EXPECT_NEAR(m_converter.rgbToOklab(RgbColor{255, 255, 255})[0], 1.0, 1e-6);
}

TEST_F(OklabConverterTest, OkhslGraysAndPrimaries) {
    expectRgb(m_converter.okhslToRgb(0, 0, 0), 0, 0, 0);
    expectRgb(m_converter.okhslToRgb(0, 0, 1), 255, 255, 255);
    expectRgb(m_converter.okhslToRgb(0, 0, 0.5), 119, 119, 119);
    // Fully saturated hue 29.2 degrees is sRGB red at the lightness of red.
    const RgbColor red = m_converter.okhslToRgb(29.23, 1, 0.568);
    EXPECT_GE(red.r, 250);
    EXPECT_LE(red.g, 5);
    EXPECT_LE(red.b, 5);
}

TEST_F(OklabConverterTest, OklchOutOfGamutKeepsHueAndLosesChroma) {
    expectRgb(m_converter.oklchToRgb(1, 0.3, 150), 255, 255, 255);
    const RgbColor inside = m_converter.oklchToRgb(0.7, 0.1, 150);
    EXPECT_GT(inside.g, inside.r);
    const RgbColor mapped = m_converter.oklchToRgb(0.7, 0.4, 150);
    EXPECT_GT(mapped.g, mapped.r);
    EXPECT_LE(mapped.g, 255);
}
