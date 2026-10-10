#!/bin/sh
# rv-emu end to end (issue #25): `malm rvemu` with a guest ELF, `malm hla`, then `malm run` through
# real pipes; stdout and the exit status must be those of `malm rvref`.
# Usage: rvemu_pipe_test.sh <malm> <elf-dir> <programs-dir> <case>
set -u
malm=$1
elf=$2
programs=$3
case_=$4
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# same <elf> <input file>: the emulator (64K of guest RAM) agrees with the reference emulator.
same() {
    "$malm" rvemu --ram 64K -o "$tmp/emu.mhl" "$1" || exit 1
    "$malm" hla -o "$tmp/emu.malm" "$tmp/emu.mhl" || exit 1
    "$malm" rvref --ram 64K "$1" < "$2" > "$tmp/ref.out"
    ref=$?
    "$malm" run "$tmp/emu.malm" < "$2" > "$tmp/emu.out"
    emu=$?
    test "$emu" = "$ref" || { echo "exit status $emu, want $ref"; exit 1; }
    cmp "$tmp/emu.out" "$tmp/ref.out" || exit 1
}

case "$case_" in
hello)
    same "$elf/hello.elf" /dev/null
    test "$(cat "$tmp/emu.out")" = "Hello, world!" || exit 1
    ;;
cat)
    # Binary-safe copy of stdin; the exit status is the byte count mod 256.
    same "$elf/cat.elf" "$programs/cat5_input.bin"
    ;;
fizzbuzz)
    same "$elf/example_fizzbuzz.elf" /dev/null
    ;;
*)
    echo "unknown case $case_"
    exit 1
    ;;
esac
