# C++ port: build and style

The C++ backbone lives next to the TypeScript packages and is built with Bazel only. It uses
C++23 named modules (`.cppm`, `export module`, `import std;`).

```
interfaces/   pure-virtual I* classes, plain data types, small pure-logic helpers (support/)
src/          implementations, one Bazel package per module
app/          composition root (the only place that wires concrete src modules together)
third_party/  pinned external dependencies and their exception-boundary bridges
tools/        module rules (modules.bzl, module.bzl), style gate
```

## Toolchain

`import std;` needs a standard library that ships the std module, so the build uses clang 20 with
libc++ (`.bazelrc` sets `CC=clang-20`):

```
apt install clang-20 libc++-20-dev libc++abi-20-dev lld-20
```

The std module is built once from libc++'s `std.cppm` (`//tools:std_module`).

## Build

```
bazelisk test //...                  # all tests (Bazel 8.4.2, see .bazelversion)
bazelisk run //tools:check_style     # style gate over the whole tree
bazelisk test --config=asan //...    # sanitizers (also --config=tsan)
```

Machine-local settings (registry mirror, `CC` override, proxy) go in `user.bazelrc` (gitignored).

Bazel has no native rule for module interface units, so `tools/modules.bzl` compiles each module
in two steps (`--precompile` to a `.pcm` BMI, then the BMI to an object) and hands every
transitive BMI to dependents with `-fmodule-file=<name>=<path>`. BMIs record absolute input
paths, so module actions run unsandboxed in the stable execroot (inputs stay declared).

## Modules

A module is one directory with one `BUILD.bazel`, `<name>.cppm` and, for non-interface modules,
`<name>_test.cpp`. The `.cppm` holds everything: `export module pi.<path>;`, `import std;`, the
exported class and its method definitions. The module name is the package path below the
top-level directory (`src/base/system_clock` -> `pi.base.system_clock`), the file name is the
snake_case of the single class. Declare modules with the macros in `tools/module.bzl`:

- `pi_interface`: module under `interfaces/` (no test of its own).
- `pi_support`: helper under `interfaces/support/`, with test. It does no I/O of its own: OS access
  goes through `I*` references. That includes the stateful orchestrators an `AgentSession` is built
  from (retry, compaction, model and bash controllers); they are support modules so the session,
  which may import only interfaces, can use them.
- `pi_module`: implementation under `src/`, with test.
- `pi_test_support`: test-only fake under `src/testing/`.

Modules `export import` the modules whose types appear in their interface, so importing a module
gives the same visibility its header used to. Third-party code (nlohmann/json, curl, BoringSSL,
POSIX headers) is `#include`d in the global module fragment (`module;` ... `export module`).
nlohmann/json must be included in every unit that uses `Json` members (template lookup).

Production modules import only `interfaces/` modules, never other `src/` modules. Tests, `app/`
and `src/testing/` may import concrete modules. Wiring happens in `app/` by constructor injection;
factories that must construct a concrete class (for example `app/agent_factory`) live there too.

## Rules (enforced by `tools/check_style.py`, `-Wall -Wextra -Werror`)

- C++23, `import std;`, no `#include` of C++ standard headers (C headers for macros are fine).
- No exceptions (`-fno-exceptions`). Fallible calls return `std::expected<T, Error>`. Third-party
  APIs that throw (yaml-cpp) are wrapped in a non-module bridge under `third_party/` that catches
  everything; a module built with exceptions cannot be imported by `-fno-exceptions` code.
- Members of classes are `m_camelCase`. Plain aggregates (a `struct` with public data and no
  methods) use plain field names. Methods camelCase, types PascalCase, interfaces `IPascalCase`,
  files and directories snake_case. 4 spaces, 100 columns (`.clang-format`).
- No `static` functions, no anonymous namespaces, no free functions. Helpers are private
  non-static methods. (De)serializers are classes (`MessageCodec`), not `to_json` overloads.
  `static constexpr` data members are allowed. `main()` and `extern "C"` plugin glue are the only
  free functions (the latter in files marked `// style:c-abi` in the first five lines).
- No class, struct, union or enum declared inside a class or function body.
- Inheritance only from `I*` interfaces. No concrete-from-concrete inheritance.
- One class per module. One task per function, at most about 40 lines (guideline).
- Test files (`*_test.cpp`) `#include <gtest/gtest.h>` first, then `import std;` and the modules
  they use; they may use `TEST` macros.
