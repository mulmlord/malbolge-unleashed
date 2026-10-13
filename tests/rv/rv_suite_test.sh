#!/bin/sh
# riscv-tests on rv-emu (issue #27) or rv-aot (issue #29): `malm build --mode <mode>` with a test
# ELF, then `malm run`. The test exits with status 0 on pass and the failing test number otherwise
# (env/riscv_test.h). With `smc`, the build must fail instead, naming self-modifying code (fence_i
# in aot mode).
# Usage: rv_suite_test.sh <malm> <emu|aot> <test.elf> [smc]
set -u
malm=$1
mode=$2
elf=$3
expect=${4:-pass}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Every test ends below 0x2000 and uses no stack: 8K of guest RAM keeps the build short.
if [ "$expect" = smc ]; then
    if "$malm" build --mode "$mode" --ram 8K -o "$tmp/test.malm" "$elf" 2> "$tmp/err"; then
        echo "$(basename "$elf"): the build succeeded"
        exit 1
    fi
    grep -q "self-modifying code is not supported in aot mode" "$tmp/err" ||
        { cat "$tmp/err"; exit 1; }
    exit 0
fi
"$malm" build --mode "$mode" --ram 8K -o "$tmp/test.malm" "$elf" || exit 1
"$malm" run "$tmp/test.malm" < /dev/null
status=$?
test "$status" = 0 || { echo "$(basename "$elf"): failed test number $status"; exit 1; }
