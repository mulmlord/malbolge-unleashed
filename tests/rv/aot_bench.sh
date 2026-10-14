#!/bin/sh
# The aot-mode benchmark (issue #31, docs/aot-benchmarks.md): every program of the three-way
# differential test (docs/rv.md 7) built with `malm build --mode aot` and run on its first input.
# Prints one Markdown table row per program: the .malm size, the length of its Malbolge text (the
# startup code, about as many steps as characters) and the VM steps of the run (startup included),
# then the totals. Wall-clock times vary too much on a busy machine; the steps do not.
# Usage: aot_bench.sh <malm> <elf dir> [<program>...]
#   <elf dir>  the test ELFs, build/<preset>/tests/rv/elf
#   <program>  hello cat example_hello example_cat wc fizzbuzz sort runtime (default: all)
set -eu
malm=$1
elf=$2
shift 2
here=$(cd "$(dirname "$0")" && pwd)
inputs="$here/inputs"
programs="$here/../programs"
[ $# -gt 0 ] || set -- hello cat example_hello example_cat wc fizzbuzz sort runtime
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "| program | guest RAM | .malm bytes | Malbolge text | VM steps |"
echo "|---|---|---|---|---|"
total_malm=0
total_text=0
total_steps=0
for name in "$@"; do
    case "$name" in
    hello) file=dump_hello ram=64K input=/dev/null ;;
    cat) file=dump_cat ram=64K input="$programs/cat5_input.bin" ;;
    example_hello) file=dump_example_hello ram=64K input=/dev/null ;;
    example_cat) file=dump_example_cat ram=64K input="$inputs/hi.in" ;;
    wc) file=dump_example_wc ram=64K input="$inputs/wc.in" ;;
    fizzbuzz) file=dump_example_fizzbuzz ram=64K input="$inputs/fizzbuzz.in" ;;
    sort) file=dump_example_sort ram=64K input="$inputs/sort.in" ;;
    runtime) file=dump_runtime_test ram=256K input="$inputs/runtime.in" ;;
    *) echo "unknown program $name" >&2; exit 2 ;;
    esac
    "$malm" build --mode aot --ram "$ram" -o "$tmp/p.malm" "$elf/$file.elf"
    "$malm" export --text -o "$tmp/p.txt" "$tmp/p.malm" 2> /dev/null
    "$malm" run --stats "$tmp/p.malm" < "$input" > /dev/null 2> "$tmp/stats" || true
    bytes=$(wc -c < "$tmp/p.malm")
    text=$(($(wc -c < "$tmp/p.txt") - 1)) # without the trailing newline
    steps=$(awk '$1 == "steps" { print $2 }' "$tmp/stats")
    echo "| $name | $ram | $bytes | $text | $steps |"
    total_malm=$((total_malm + bytes))
    total_text=$((total_text + text))
    total_steps=$((total_steps + steps))
done
echo "| total | | $total_malm | $total_text | $total_steps |"
