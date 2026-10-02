"""Module macros. One module = one directory = one class (see docs/cpp-style.md)."""

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

PI_COPTS = [
    "-Wall",
    "-Wextra",
    "-Werror",
    "-fno-exceptions",
]

def pi_interface(name, deps = [], srcs = []):
    """Interface or plain data type module under //interfaces. Visible everywhere."""
    cc_library(
        name = name,
        hdrs = [name + ".h"],
        srcs = srcs,
        deps = deps,
        copts = PI_COPTS,
        visibility = ["//visibility:public"],
    )

def pi_module(name, deps = [], test_deps = [], data = []):
    """Implementation module under //src. Only //app may depend on it.

    The directory must hold <name>.h, <name>.cpp and <name>_test.cpp.
    """
    cc_library(
        name = name,
        hdrs = [name + ".h"],
        srcs = [name + ".cpp"],
        deps = deps,
        copts = PI_COPTS,
        visibility = ["//app:__subpackages__"],
    )
    cc_test(
        name = name + "_test",
        srcs = [name + "_test.cpp"],
        deps = [":" + name, "@googletest//:gtest_main"] + test_deps,
        copts = PI_COPTS,
        data = data,
    )
