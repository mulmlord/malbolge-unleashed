#!/bin/sh
# Real-pipe tests for `malm rvref` on the clang-built test programs.
# Usage: rvref_pipe_test.sh <malm> <elf-dir> <programs-dir> <case>
set -u
malm=$1
elf=$2
programs=$3
case_=$4
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

case "$case_" in
hello)
    out=$("$malm" rvref "$elf/hello.elf") || exit 1
    test "$out" = "Hello, world!" || exit 1
    ;;
cat)
    # Binary-safe copy of stdin; the exit status is the byte count mod 256.
    size=$(wc -c < "$programs/cat5_input.bin")
    ( "$malm" rvref "$elf/cat.elf" < "$programs/cat5_input.bin"; echo $? > "$tmp/status" ) \
        | cmp - "$programs/cat5_input.bin" || exit 1
    test "$(cat "$tmp/status")" = $((size % 256)) || exit 1
    "$malm" rvref "$elf/cat.elf" < /dev/null > "$tmp/out" || exit 1
    test ! -s "$tmp/out" || exit 1
    ;;
trace)
    # stdout is unchanged; stderr has one line per instruction, ending with the exit ecall.
    out=$("$malm" rvref --trace "$elf/hello.elf" 2> "$tmp/err") || exit 1
    test "$out" = "Hello, world!" || exit 1
    test "$(head -n 1 "$tmp/err" | cut -d' ' -f1-2)" = "step=1 pc=0x00001000" || exit 1
    tail -n 1 "$tmp/err" | grep -q ' insn=0x00000073$' || exit 1
    test "$(grep -vc '^step=' "$tmp/err")" = 0 || exit 1
    ;;
regs)
    # sp starts at the top of guest RAM, so --ram shows up in the final registers.
    "$malm" rvref --regs --ram 64K "$elf/hello.elf" > /dev/null 2> "$tmp/err" || exit 1
    grep -q '^x2 sp    0x00010000$' "$tmp/err" || exit 1
    grep -q '^x17 a7   0x0000005d$' "$tmp/err" || exit 1
    test "$(grep -c '^x[0-9]' "$tmp/err")" = 32 || exit 1
    ;;
*)
    echo "unknown case $case_" >&2
    exit 2
    ;;
esac
