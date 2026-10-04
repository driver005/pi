#!/bin/sh
# Drives the C++ `pi serve` with the TypeScript client and durable storage (manual cross-check, needs
# `npm install --ignore-scripts` and a built bazel-bin/app/pi/pi). Usage: tools/ts_crosscheck/run.sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export NODE_PATH="$root/node_modules"
export PI_BINARY="${PI_BINARY:-$root/bazel-bin/app/pi/pi}"
esbuild="$root/node_modules/.bin/esbuild"
bundle() {
    "$esbuild" "$root/tools/ts_crosscheck/$1.mts" --bundle --platform=node --format=esm --conditions=source \
        --external:node:sqlite \
        --alias:@earendil-works/pi-protocol="$root/packages/protocol/src/index.ts" \
        --alias:@earendil-works/pi-client="$root/packages/client/src/index.ts" \
        --alias:@earendil-works/pi-client/unix="$root/packages/client/src/unix.ts" \
        --outfile="$work/$1.mjs" --log-level=error
}
for name in client_check import_check copy_session_db; do bundle "$name"; done
node "$work/client_check.mjs" tree
output=$(node "$work/client_check.mjs" durable | tee /dev/stderr)
db=$(printf '%s\n' "$output" | sed -n 's/^DB //p')
node --no-warnings "$work/copy_session_db.mjs" "$db" "$work/ts_written.sqlite"
node "$work/import_check.mjs" "$work/ts_written.sqlite"
echo "ts cross-check passed"
