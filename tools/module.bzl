"""Module macros. One module = one directory = one class (see docs/cpp-style.md)."""

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

PI_COPTS = [
    "-Wall",
    "-Wextra",
    "-Werror",
    "-fno-exceptions",
]

# Only for modules that wrap a throwing third-party API (e.g. yaml-cpp) and must catch at the
# boundary. Keep these modules tiny; everything else is built with -fno-exceptions.
PI_COPTS_WITH_EXCEPTIONS = [
    "-Wall",
    "-Wextra",
    "-Werror",
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
        deps = [":" + name, "//third_party:gtest"] + test_deps,
        copts = PI_COPTS,
        data = data,
    )

def pi_support(name, deps = [], test_deps = [], header_only = False, allow_exceptions = False):
    """Pure-logic helper class under //interfaces/support: no I/O, no OS state, public.

    Callers may instantiate these directly (AbortSignal, EventStream, SseParser, ...) because
    there is nothing worth mocking. Anything touching the OS, network or disk goes behind an
    I* interface with an implementation in //src.
    """
    cc_library(
        name = name,
        hdrs = [name + ".h"],
        srcs = [] if header_only else [name + ".cpp"],
        deps = deps,
        copts = PI_COPTS_WITH_EXCEPTIONS if allow_exceptions else PI_COPTS,
        visibility = ["//visibility:public"],
    )
    cc_test(
        name = name + "_test",
        srcs = [name + "_test.cpp"],
        deps = [":" + name, "//third_party:gtest"] + test_deps,
        copts = PI_COPTS,
    )

def pi_test_support(name, deps = [], srcs = []):
    """Test-only helper module (fakes, fixtures). Public so any test can use it."""
    cc_library(
        name = name,
        hdrs = [name + ".h"],
        srcs = srcs,
        deps = deps,
        copts = PI_COPTS,
        testonly = True,
        visibility = ["//visibility:public"],
    )
