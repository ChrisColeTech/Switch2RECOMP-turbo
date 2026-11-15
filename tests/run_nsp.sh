#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
testdir=$(mktemp -d)
trap 'rm -rf "$testdir"' EXIT
printf 'xxxxtestxxxx' > "$testdir/package.nsp"
printf 'SWRNSP1\n%s\n4\n4\n' "$testdir/package.nsp" > "$testdir/romfs.nsp"
cc -O1 -g -fsanitize=address,undefined -Iruntime tests/test_nsp_runtime.c runtime/nsp.c -o "$testdir/nsp"
ASAN_OPTIONS=detect_leaks=1 "$testdir/nsp" "$testdir/romfs.nsp"
