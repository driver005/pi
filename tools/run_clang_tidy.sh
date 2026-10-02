#!/usr/bin/env bash
# Runs clang-tidy (naming, function size) over the C++ sources.
# Usage: tools/run_clang_tidy.sh [files...]   (default: every .cpp under src/ app/ plugins/ sdk/)
set -euo pipefail
cd "$(dirname "$0")/.."

BAZEL="${BAZEL:-bazelisk}"
output_base="$($BAZEL info output_base 2>/dev/null)"
includes=(-I.)
for dir in "$output_base"/external/*/include "$output_base"/external/googletest*/googletest/include; do
    [ -d "$dir" ] && includes+=("-isystem" "$dir")
done

if [ "$#" -gt 0 ]; then
    files=("$@")
else
    mapfile -t files < <(find src app plugins sdk -name '*.cpp' ! -name '*_test.cpp' 2>/dev/null | sort)
fi
[ "${#files[@]}" -eq 0 ] && { echo "no files"; exit 0; }
clang-tidy "${files[@]}" -- -std=c++23 -fno-exceptions "${includes[@]}"
