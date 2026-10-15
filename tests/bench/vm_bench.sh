#!/bin/sh
# VM benchmark (issue #37, docs/benchmarks.md): builds the benchmark programs and times the VM's
# step loop on each with vm_bench. Prints one Markdown table row per program, then the total.
#
# Usage: vm_bench.sh <malm> <vm_bench> [<baseline vm_bench>]
#
# With a baseline (vm_bench built from an older commit), each program runs on the baseline and on
# <vm_bench> back to back, three times alternately, and the fastest run of each counts: wall-clock
# rates on a busy machine are only comparable when measured together. The programs are built once,
# with <malm>, and both tools run the same files.
set -eu

malm=$1
bench=$2
base=${3:-}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

printf '123 456\n' > "$tmp/adder.in"
printf '9875\n' > "$tmp/root.in"
printf 'Hello, cat!\n' > "$tmp/cat.in"
printf 'Hello, rev!' > "$tmp/rev.in"
cp "$root/tests/rv/inputs/fizzbuzz.in" "$tmp/fizzbuzz.in"
: > "$tmp/empty.in"

# name|mode|build command writing "$out"|stdin file in $tmp
cat > "$tmp/programs" <<'EOF'
beer99|classic|cp "$root/tests/programs/beer99.mal" "$out"|empty.in
asm hello|unleashed|"$malm" asm "$root/examples/asm/hello.mua" -o "$out"|empty.in
asm adder|unleashed|"$malm" asm "$root/examples/asm/adder.mua" -o "$out"|adder.in
asm digital_root|unleashed|"$malm" asm "$root/examples/asm/digital_root.mua" -o "$out"|root.in
asm halting_cat|unleashed|"$malm" asm "$root/examples/asm/halting_cat.mua" -o "$out"|cat.in
hla hello|unleashed|"$malm" hla "$root/examples/hla/hello.mhl" -o "$out"|empty.in
hla rev|unleashed|"$malm" hla "$root/examples/hla/rev.mhl" -o "$out"|rev.in
hla cat|unleashed|"$malm" hla "$root/examples/hla/cat.mhl" -o "$out"|cat.in
C hello, emu|unleashed|"$malm" build "$root/examples/c/hello.c" -o "$out"|empty.in
C fizzbuzz, emu|unleashed|"$malm" build "$root/examples/c/fizzbuzz.c" -o "$out"|fizzbuzz.in
C hello, aot|unleashed|"$malm" build --mode aot "$root/examples/c/hello.c" -o "$out"|empty.in
EOF

# The faster of two vm_bench lines (5 fields each; field 2 is the seconds of the fastest run). The
# first may be empty.
faster() {
    if [ -z "$1" ] || [ "$(echo "$1 $2" | awk '{ print ($7 < $2) }')" = 1 ]; then
        echo "$2"
    else
        echo "$1"
    fi
}

if [ -n "$base" ]; then
    echo "| program | mode | VM steps | steps/s before | steps/s after | speed-up |"
    echo "|---|---|---|---|---|---|"
else
    echo "| program | mode | VM steps | steps/s |"
    echo "|---|---|---|---|"
fi
: > "$tmp/results"
while IFS='|' read -r name mode cmd input; do
    out="$tmp/p.malm"
    rm -f "$out"
    eval "$cmd" 2> /dev/null
    flag=
    [ "$mode" = classic ] && flag=--classic
    new=
    old=
    for _ in 1 2 3; do
        [ -z "$base" ] || old=$(faster "$old" "$("$base" $flag "$out" "$tmp/$input")")
        new=$(faster "$new" "$("$bench" $flag "$out" "$tmp/$input")")
    done
    [ -n "$base" ] || old=$new
    echo "$name|$mode|$old|$new" >> "$tmp/results"
done < "$tmp/programs"

awk -F'|' -v cmp="$base" '{
    split($3, o, " "); split($4, n, " ")
    if (o[1] != n[1]) { print "step counts differ for " $1 > "/dev/stderr"; exit 1 }
    steps += n[1]; old_s += o[2]; new_s += n[2]
    if (cmp != "") {
        printf "| %s | %s | %d | %.1f M | %.1f M | %.2fx |\n", $1, $2, n[1], o[3] / 1e6, n[3] / 1e6,
               n[3] / o[3]
    } else {
        printf "| %s | %s | %d | %.1f M |\n", $1, $2, n[1], n[3] / 1e6
    }
}
END {
    if (cmp != "") {
        printf "| total | | %d | %.1f M | %.1f M | %.2fx |\n", steps, steps / old_s / 1e6,
               steps / new_s / 1e6, old_s / new_s
    } else {
        printf "| total | | %d | %.1f M |\n", steps, steps / new_s / 1e6
    }
}' "$tmp/results"
