#!/bin/sh
# riscv-tests on rv-emu (issue #27): `malm build` with a test ELF, then `malm run`. The test exits
# with status 0 on pass and the failing test number otherwise (env/riscv_test.h).
# Usage: rvemu_suite_test.sh <malm> <test.elf>
set -u
malm=$1
elf=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Every test ends below 0x2000 and uses no stack: 8K of guest RAM keeps the build short.
"$malm" build --ram 8K -o "$tmp/test.malm" "$elf" || exit 1
"$malm" run "$tmp/test.malm" < /dev/null
status=$?
test "$status" = 0 || { echo "$(basename "$elf"): failed test number $status"; exit 1; }
