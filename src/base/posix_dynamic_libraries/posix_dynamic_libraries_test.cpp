#include <gtest/gtest.h>

import std;
import pi.base.posix_dynamic_libraries;

TEST(PosixDynamicLibrariesTest, OpensLibrariesAndResolvesSymbols) {
    PosixDynamicLibraries libraries;
    const auto library = libraries.open("libm.so.6");
    ASSERT_TRUE(library.has_value());
    const auto cosine = libraries.symbol(*library, "cos");
    ASSERT_TRUE(cosine.has_value());
    const auto function = reinterpret_cast<double (*)(double)>(*cosine);
    EXPECT_DOUBLE_EQ(function(0.0), 1.0);
    libraries.close(*library);
    EXPECT_FALSE(libraries.symbol(*library, "cos").has_value());
}

TEST(PosixDynamicLibrariesTest, ReportsMissingLibrariesAndSymbols) {
    PosixDynamicLibraries libraries;
    const auto missing = libraries.open("/definitely/not/here.so");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, "dlopen");
    EXPECT_FALSE(missing.error().message.empty());
    const auto library = libraries.open("libm.so.6");
    ASSERT_TRUE(library.has_value());
    const auto symbol = libraries.symbol(*library, "no_such_symbol_xyz");
    ASSERT_FALSE(symbol.has_value());
    EXPECT_EQ(symbol.error().message, "missing symbol no_such_symbol_xyz");
}
