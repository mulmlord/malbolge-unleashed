#!/bin/sh
# `malm build` for C and C++ end to end (issues #28, #29, #32, #33): an example from examples/c or
# examples/cpp (<case>.c or <case>.cpp) built in emu or aot mode, with the minimal C library or
# picolibc, into a .malm and, with --exe, a native executable; both run through real pipes and must
# give the expected stdout and exit status.
# Usage: build_pipe_test.sh <malm> <cc> <examples-dir> <programs-dir> <case> [emu|aot]
#            [minimal|picolibc]
set -u
malm=$1
cc=$2
examples=$3
programs=$4
case_=$5
mode=${6:-emu}
libc=${7:-minimal}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
src=$examples/$case_.c
if [ ! -e "$src" ]; then
    src=$examples/$case_.cpp
fi

# build [compiler args...]: prog.malm and the executable prog. 64K of guest RAM keeps the builds
# short.
build() {
    set -- --mode "$mode" --libc "$libc" --ram 64K --cc "$cc" "$src" -- "$@"
    "$malm" build -o "$tmp/prog.malm" "$@" || exit 1
    "$malm" build --exe -o "$tmp/prog" "$@" 2> "$tmp/err" || { cat "$tmp/err"; exit 1; }
}

# check <input file> <want file> <want status>: `cat input | malm run prog.malm` and
# `cat input | ./prog` both write <want> and exit with <status>.
check() {
    for how in run exe; do
        status=0
        if [ "$how" = run ]; then
            cat "$1" | "$malm" run "$tmp/prog.malm" > "$tmp/out" || status=$?
        else
            cat "$1" | "$tmp/prog" > "$tmp/out" || status=$?
        fi
        test "$status" = "$3" || { echo "$how: exit status $status, want $3"; exit 1; }
        cmp "$tmp/out" "$2" || { echo "$how: wrong output"; exit 1; }
    done
}

case "$case_" in
hello)
    build
    echo 'Hello, world!' > "$tmp/want"
    check /dev/null "$tmp/want" 0
    ;;
cat)
    # The M3 exit criterion: `echo hi | ./cat` echoes hi. Also binary input and none.
    build
    echo hi > "$tmp/in"
    check "$tmp/in" "$tmp/in" 0
    check "$programs/cat5_input.bin" "$programs/cat5_input.bin" 0
    check /dev/null /dev/null 0
    ;;
wc)
    build
    printf 'one two\n  three\t\nfour' > "$tmp/in"
    wc < "$tmp/in" | awk '{ print $1, $2, $3 }' > "$tmp/want"
    check "$tmp/in" "$tmp/want" 0
    ;;
fizzbuzz)
    # main's return value is the exit status: 0, or 2 with a message on stderr (merged into
    # stdout) for bad input.
    build
    echo 15 > "$tmp/in"
    printf '1\n2\nFizz\n4\nBuzz\nFizz\n7\n8\nFizz\nBuzz\n11\nFizz\n13\n14\nFizzBuzz\n' > "$tmp/want"
    check "$tmp/in" "$tmp/want" 0
    echo nope > "$tmp/in"
    echo 'fizzbuzz: expected a number from 1 to 10000' > "$tmp/want"
    check "$tmp/in" "$tmp/want" 2
    ;;
sort)
    # The heap stays __stack_size (default 64K) below the stack, which would leave sort no heap in
    # 64K of guest RAM.
    build -Wl,--defsym=__stack_size=16384
    printf 'pear\nApple\n\napple\n\303\244\npear\nfig' > "$tmp/in"
    LC_ALL=C sort < "$tmp/in" > "$tmp/want"
    check "$tmp/in" "$tmp/want" 0
    ;;
stats)
    # picolibc only: floating point, scanf and sqrt.
    build
    printf '1.5 -2.25\n1e3\n' > "$tmp/in"
    printf 'count 3\nmin -2.25\nmax 1000\nmean 333.083\nstddev 471.584\n' > "$tmp/want"
    check "$tmp/in" "$tmp/want" 0
    ;;
*)
    echo "unknown case $case_" >&2
    exit 2
    ;;
esac
