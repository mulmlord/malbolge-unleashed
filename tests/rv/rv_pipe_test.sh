#!/bin/sh
# rv-emu and rv-aot end to end (issues #25, #26, #29): `malm build --mode <mode>` with a guest ELF,
# then `malm run` through real pipes; stdout and the exit status must be those of `malm rvref`.
# Usage: rv_pipe_test.sh <malm> <emu|aot> <elf-dir> <programs-dir> <case>
set -u
malm=$1
mode=$2
elf=$3
programs=$4
case_=$5
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# same <elf> <input file> [guest RAM]: the program built in the mode (64K of guest RAM by default)
# agrees with the reference emulator.
same() {
    ram=${3:-64K}
    "$malm" build --mode "$mode" --ram "$ram" -o "$tmp/prog.malm" "$1" || exit 1
    "$malm" rvref --ram "$ram" "$1" < "$2" > "$tmp/ref.out"
    ref=$?
    "$malm" run "$tmp/prog.malm" < "$2" > "$tmp/prog.out"
    got=$?
    test "$got" = "$ref" || { echo "exit status $got, want $ref"; exit 1; }
    cmp "$tmp/prog.out" "$tmp/ref.out" || exit 1
}

case "$case_" in
hello)
    same "$elf/hello.elf" /dev/null
    test "$(cat "$tmp/prog.out")" = "Hello, world!" || exit 1
    ;;
cat)
    # Binary-safe copy of stdin; the exit status is the byte count mod 256.
    same "$elf/cat.elf" "$programs/cat5_input.bin"
    ;;
fizzbuzz)
    same "$elf/example_fizzbuzz.elf" /dev/null
    ;;
runtime)
    # The runtime's self-check (it needs 256K of guest RAM for its heap); exit(42) on success.
    printf 'line1\nxyz' > "$tmp/in"
    same "$elf/runtime_test.elf" "$tmp/in" 256K
    test "$ref" = 42 || exit 1
    ;;
*)
    echo "unknown case $case_"
    exit 1
    ;;
esac
