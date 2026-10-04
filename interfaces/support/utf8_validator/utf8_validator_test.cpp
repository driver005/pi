#include <gtest/gtest.h>

import std;
import pi.support.utf8_validator;

TEST(Utf8ValidatorTest, AcceptsWellFormedText) {
    const Utf8Validator validator;
    EXPECT_TRUE(validator.valid(""));
    EXPECT_TRUE(validator.valid("plain ascii"));
    EXPECT_TRUE(validator.valid("\xC3\xBC"));
    EXPECT_TRUE(validator.valid("\xE6\xB0\xB4"));
    EXPECT_TRUE(validator.valid("\xF0\x90\x85\x91"));
    EXPECT_TRUE(validator.valid("\xEF\xBB\xBF"));
}

TEST(Utf8ValidatorTest, RejectsMalformedText) {
    const Utf8Validator validator;
    EXPECT_FALSE(validator.valid("\xFF"));
    EXPECT_FALSE(validator.valid("\xC0\x80"));
    EXPECT_FALSE(validator.valid("\xED\xA0\x80"));
    EXPECT_FALSE(validator.valid("\xF4\x90\x80\x80"));
    EXPECT_FALSE(validator.valid("\xE0\x80\x80"));
    EXPECT_FALSE(validator.valid("\xC3"));
    EXPECT_FALSE(validator.valid("\x80"));
    EXPECT_FALSE(validator.valid("a\xE6\xB0"));
}
