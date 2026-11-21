#!/bin/sh
set -eu

CC="${CC:-cc}"
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

"$CC" -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$ROOT/host/ps5" \
    "$ROOT/host/ps5/memory_state.c" \
    "$ROOT/host/ps5/memory_state_test.c" \
    -o "$TMP/memory_state_test"
"$TMP/memory_state_test"
printf '%s\n' "PS5 memory-state host tests passed"
