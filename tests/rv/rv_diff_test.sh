#!/bin/sh
# The three-way differential test (issue #30, docs/rv.md 7): a guest program on the reference
# emulator (`malm rvref`), built in emu mode and in aot mode (`malm build`, `malm run`), each input
# through real pipes. stdout and the exit status must be the same on all three. The program dumps
# its registers before it exits (ecall 255: riscv-tests through env/riscv_test.h, programs linked
# with --defsym=__malm_dump_regs=1), so stdout compares the final registers too.
# Usage: rv_diff_test.sh <malm> <elf> <ram> [--status N] [--aot-smc] [--] <input file>...
#   --status N  the reference emulator must also exit with N
#   --aot-smc   aot mode must refuse the program as self-modifying code; emu mode is still compared
set -u
malm=$1
elf=$2
ram=$3
shift 3
status=""
aot_smc=no
while [ $# -gt 0 ]; do
    case "$1" in
    --status) status=$2; shift 2 ;;
    --aot-smc) aot_smc=yes; shift ;;
    --) shift; break ;;
    *) break ;;
    esac
done
[ $# -gt 0 ] || { echo "no input files" >&2; exit 2; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
name=$(basename "$elf")

modes=emu
"$malm" build --mode emu --ram "$ram" -o "$tmp/emu.malm" "$elf" || exit 1
if [ "$aot_smc" = yes ]; then
    if "$malm" build --mode aot --ram "$ram" -o "$tmp/aot.malm" "$elf" 2> "$tmp/err"; then
        echo "$name: aot mode built a self-modifying program"
        exit 1
    fi
    grep -q "self-modifying code is not supported in aot mode" "$tmp/err" ||
        { cat "$tmp/err"; exit 1; }
else
    "$malm" build --mode aot --ram "$ram" -o "$tmp/aot.malm" "$elf" || exit 1
    modes="emu aot"
fi

for input in "$@"; do
    "$malm" rvref --ram "$ram" "$elf" < "$input" > "$tmp/ref.out"
    ref=$?
    if [ -n "$status" ] && [ "$ref" != "$status" ]; then
        echo "$name < $input: the reference emulator exits with $ref, want $status"
        exit 1
    fi
    grep -aq '== registers ==$' "$tmp/ref.out" ||
        { echo "$name < $input: no register dump in the output"; exit 1; }
    for mode in $modes; do
        "$malm" run "$tmp/$mode.malm" < "$input" > "$tmp/$mode.out"
        got=$?
        if [ "$got" != "$ref" ]; then
            echo "$name < $input: $mode mode exits with $got, the reference emulator with $ref"
            exit 1
        fi
        if ! cmp -s "$tmp/$mode.out" "$tmp/ref.out"; then
            echo "$name < $input: $mode mode's output differs (< reference, > $mode):"
            diff -a "$tmp/ref.out" "$tmp/$mode.out" | head -n 40
            exit 1
        fi
    done
done
