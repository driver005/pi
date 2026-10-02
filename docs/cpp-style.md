# C++ port: build and style

The C++ backbone lives next to the TypeScript packages and is built with Bazel only.

```
interfaces/   pure-virtual I* classes and plain data types (header-only, public)
src/          implementations, one Bazel package per module
app/          composition root, the only package that may depend on src/
third_party/  pinned external dependencies
tools/        module macros, style gate
```

## Build

```
bazelisk test //...                  # all tests (Bazel 8.4.2, see .bazelversion)
bazelisk run //tools:check_style     # style gate over the whole tree
tools/run_clang_tidy.sh              # naming and function size (needs clang-tidy)
bazelisk test --config=asan //...    # sanitizers (also --config=tsan)
```

Machine-local settings (registry mirror, proxy) go in `user.bazelrc` (gitignored).

## Modules

A module is one directory with one `BUILD.bazel`, `<name>.h`, `<name>.cpp`, `<name>_test.cpp`
and exactly one class. The directory and header name are the snake_case of the class name
(`FileMutationQueue` -> `file_mutation_queue/file_mutation_queue.h`). Declare modules with the
macros in `tools/module.bzl`:

- `pi_interface`: header-only module under `interfaces/`, visible everywhere.
- `pi_module`: implementation under `src/`, test included, visible only to `//app`.

Implementations depend on `//interfaces/...` and `//third_party/...` only. Wiring happens in
`app/` by constructor injection of interface references. Including another module's `src/`
header is a style violation.

## Rules (enforced by `tools/check_style.py`, clang-tidy, `-Wall -Wextra -Werror`)

- C++23, no exceptions (`-fno-exceptions`). Fallible calls return `std::expected<T, Error>`.
- Members of classes are `m_camelCase`. Plain aggregates (a `struct` with public data and no
  methods) use plain field names. Methods camelCase, types PascalCase, interfaces `IPascalCase`,
  files and directories snake_case. 4 spaces, 100 columns (`.clang-format`).
- No `static` functions, no anonymous namespaces, no free functions. Helpers are private
  non-static methods. (De)serializers are classes (`MessageCodec`), not `to_json` overloads.
  `static constexpr` data members are allowed. `main()` and `extern "C"` plugin glue are the only
  free functions (the latter in files marked `// style:c-abi` in the first five lines).
- No class, struct, union or enum declared inside a class or function body.
- Inheritance only from `I*` interfaces. No concrete-from-concrete inheritance.
- One class per module. One task per function, at most about 40 lines.
- `#pragma once` in headers. Test files (`*_test.cpp`) may use `TEST` macros.
