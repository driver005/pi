"""Module macros. One module = one directory = one class = one <name>.cppm (see docs/cpp-style.md).

The module name is derived from the package path below the top-level directory:
interfaces/types/json -> pi.types.json, src/base/system_clock -> pi.base.system_clock.
"""

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")
load(":modules.bzl", "pi_cc_module", "pi_cc_tu")

PI_COPTS = [
    "-Wall",
    "-Wextra",
    "-Werror",
    "-fno-exceptions",
]

_GTEST = "//third_party:gtest"

def _module_name():
    parts = native.package_name().split("/")
    return "pi." + ".".join(parts[1:])

def _module(name, deps, testonly = False):
    pi_cc_module(
        name = name,
        src = name + ".cppm",
        module_name = _module_name(),
        deps = deps,
        copts = PI_COPTS,
        testonly = testonly,
        visibility = ["//visibility:public"],
    )

def _test(name, test_deps, data = []):
    pi_cc_tu(
        name = name + "_test_tu",
        src = name + "_test.cpp",
        deps = [":" + name, _GTEST] + test_deps,
        copts = PI_COPTS,
        testonly = True,
    )
    cc_test(
        name = name + "_test",
        srcs = [":" + name + "_test_tu.o"],
        deps = [":" + name, _GTEST] + test_deps,
        data = data,
    )

def pi_interface(name, deps = []):
    """Interface or plain data type module under //interfaces. Has no test of its own."""
    _module(name, deps)

def pi_support(name, deps = [], test_deps = [], header_only = False):
    """Pure-logic helper class under //interfaces/support: no I/O, no OS state.

    Callers may instantiate these directly (AbortSignal, EventStream, SseParser, ...) because
    there is nothing worth mocking. Anything touching the OS, network or disk goes behind an
    I* interface with an implementation in //src. `header_only` is accepted for old BUILD files.
    """
    _module(name, deps)
    _test(name, test_deps)

def pi_module(name, deps = [], test_deps = [], data = []):
    """Implementation module under //src, with <name>_test.cpp next to it.

    Production code may import another src module only through its //interfaces module; the style
    gate (tools/check_style.py) enforces that. Bazel visibility stays public so integration tests
    and the composition root can link concrete modules.
    """
    _module(name, deps)
    _test(name, test_deps, data)

def pi_test_support(name, deps = [], srcs = []):
    """Test-only helper module (fakes, fixtures). Public so any test can use it."""
    _module(name, deps, testonly = True)

def pi_binary(name, deps = []):
    """Executable under //app whose main() lives in <name>.cpp and imports modules."""
    pi_cc_tu(
        name = name + "_tu",
        src = name + ".cpp",
        deps = deps,
        copts = PI_COPTS,
    )
    cc_binary(
        name = name,
        srcs = [":" + name + "_tu.o"],
        deps = deps,
    )
