#!/bin/sh
# The HLA examples through the real CLI and real pipes (issue #19): `malm hla` to a .malm, then
# `malm run` with piped stdin, compared with the assembly examples; plus --check, --print and
# --text.
# Usage: examples_pipe_test.sh <malm> <hla-examples-dir> <asm-examples-dir> <case>
set -u
malm=$1
dir=$2
asmdir=$3
case_=$4
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Diagnostics, exit codes, and no output file after an error.
if [ "$case_" = errors ]; then
    printf 'reg a\n  mov b, a\n  jmp a\n' > "$tmp/bad.mhl"
    "$malm" hla --check "$tmp/bad.mhl" 2> "$tmp/err"
    test $? = 1 || exit 1
    grep -q "bad.mhl:2:7: error: unknown register 'b'" "$tmp/err" || exit 1
    grep -q "bad.mhl:3:7: error: 'a' is a register, not a label" "$tmp/err" || exit 1
    "$malm" hla "$tmp/bad.mhl" 2> /dev/null
    test $? = 2 || exit 1
    "$malm" hla "$tmp/bad.mhl" -o "$tmp/bad.malm" 2> /dev/null
    test $? = 1 || exit 1
    test ! -e "$tmp/bad.malm" || exit 1
    exit 0
fi

"$malm" hla "$dir/$case_.mhl" -o "$tmp/p.malm" || exit 1
"$malm" hla --check "$dir/$case_.mhl" || exit 1

# The printed assembly assembles to the same program text.
"$malm" hla --print "$dir/$case_.mhl" > "$tmp/p.mua" || exit 1
"$malm" asm --text "$tmp/p.mua" -o "$tmp/a.mal" || exit 1
"$malm" hla --text "$dir/$case_.mhl" -o "$tmp/h.mal" 2> "$tmp/warn" || exit 1
cmp "$tmp/a.mal" "$tmp/h.mal" || exit 1
grep -q EXIT_A "$tmp/warn" || exit 1

case "$case_" in
hello)
    "$malm" asm "$asmdir/hello.mua" -o "$tmp/a.malm" || exit 1
    "$malm" run "$tmp/a.malm" < /dev/null > "$tmp/want" || exit 1
    "$malm" run "$tmp/p.malm" < /dev/null > "$tmp/out" || exit 1
    cmp "$tmp/want" "$tmp/out" || exit 1
    ;;
cat)
    "$malm" asm "$asmdir/halting_cat.mua" -o "$tmp/a.malm" || exit 1
    i=0
    while [ $i -lt 256 ]; do
        printf "\\$(printf %o $i)"
        i=$((i + 1))
    done > "$tmp/all.bin"
    for input in "$tmp/all.bin" /dev/null; do
        "$malm" run "$tmp/a.malm" < "$input" > "$tmp/want" || exit 1
        "$malm" run "$tmp/p.malm" < "$input" > "$tmp/out" || exit 1
        cmp "$tmp/want" "$tmp/out" || exit 1
    done
    printf 'Hello, cat!\n' | "$malm" run "$tmp/p.malm" > "$tmp/out" || exit 1
    printf 'Hello, cat!\n' | cmp - "$tmp/out" || exit 1
    ;;
exit42)
    "$malm" run "$tmp/p.malm" < /dev/null
    test $? = 42 || exit 1
    # Text cannot carry EXIT_A: the halt code needs --exit-a.
    "$malm" run "$tmp/h.mal" < /dev/null
    test $? = 0 || exit 1
    "$malm" run --exit-a "$tmp/h.mal" < /dev/null
    test $? = 42 || exit 1
    "$malm" pack -o "$tmp/exe" "$tmp/p.malm" 2> /dev/null || exit 1
    "$tmp/exe" < /dev/null
    test $? = 42 || exit 1
    ;;
*)
    echo "unknown case $case_" >&2
    exit 2
    ;;
esac
