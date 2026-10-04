#include <gtest/gtest.h>

import std;
import pi.support.js_number_format;

TEST(JsNumberFormatTest, ToFixedRoundsTiesAwayFromZeroLikeJavaScript) {
    const JsNumberFormat format;
    EXPECT_EQ(format.toFixed(0.5, 0), "1");
    EXPECT_EQ(format.toFixed(2.5, 0), "3");
    EXPECT_EQ(format.toFixed(0.25, 1), "0.3");
    EXPECT_EQ(format.toFixed(3.25, 1), "3.3");
    EXPECT_EQ(format.toFixed(-3.25, 1), "-3.3");
    EXPECT_EQ(format.toFixed(1.005, 2), "1.00") << "1.005 is below the tie in binary";
    EXPECT_EQ(format.toFixed(12.0, 1), "12.0");
    EXPECT_EQ(format.toFixed(49174.85, 1), "49174.8") << "the double is below the decimal tie";
    EXPECT_EQ(format.toFixed(49174.75, 1), "49174.8");
    EXPECT_EQ(format.toFixed(-9402.65, 1), "-9402.6");
    EXPECT_EQ(format.toFixed(8.345, 2), "8.35");
    EXPECT_EQ(format.toFixed(10.235, 2), "10.23");
    EXPECT_EQ(format.toFixed(1.45, 1), "1.4");
    EXPECT_EQ(format.toFixed(-0.0001, 1), "-0.0");
    EXPECT_EQ(format.toFixed(0.0, 2), "0.00");
}

TEST(JsNumberFormatTest, RoundsToFifteenSignificantDigits) {
    const JsNumberFormat format;
    EXPECT_EQ(format.roundTo15(0.30000000000000004), 0.3);
    EXPECT_EQ(format.roundTo15(0.75 - 0.5), 0.25);
    EXPECT_EQ(format.roundTo15(1234.5), 1234.5);
    EXPECT_EQ(format.roundTo15(0.0), 0.0);
}
