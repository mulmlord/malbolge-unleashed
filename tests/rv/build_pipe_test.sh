#!/bin/sh
# `malm build` for C end to end (issue #28): an example from examples/c built into a .malm and,
# with --exe, a native executable; both run through real pipes and must give the expected stdout
# and exit status.
# Usage: build_pipe_test.sh <malm> <cc> <examples-dir> <programs-dir> <case>
set -u
malm=$1
cc=$2
examples=$3
programs=$4
case_=$5
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# build [compiler args...]: prog.malm and the executable prog. 64K of guest RAM keeps the builds
# short.
build() {
    "$malm" build --ram 64K --cc "$cc" -o "$tmp/prog.malm" "$examples/$case_.c" -- "$@" || exit 1
    "$malm" build --ram 64K --cc "$cc" --exe -o "$tmp/prog" "$examples/$case_.c" -- "$@" \
        2> "$tmp/err" || { cat "$tmp/err"; exit 1; }
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
*)
    echo "unknown case $case_" >&2
    exit 2
    ;;
esac
