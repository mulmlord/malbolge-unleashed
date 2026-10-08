#!/bin/sh
# The golden examples through the real CLI and real pipes (issue #16): `malm asm` to a .malm, then
# `malm run` with piped stdin. Usage: examples_pipe_test.sh <malm> <examples-dir> <case>
set -u
malm=$1
dir=$2
case_=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$malm" asm "$dir/$case_.mua" -o "$tmp/p.malm" || exit 1

# Runs the program on stdin; the output must equal $1 byte for byte and the exit status be 0.
expect() {
    ( "$malm" run "$tmp/p.malm"; echo $? > "$tmp/status" ) > "$tmp/out" || exit 1
    printf "$1" | cmp - "$tmp/out" || exit 1
    test "$(cat "$tmp/status")" = 0 || { echo "exit status $(cat "$tmp/status")" >&2; exit 1; }
}

case "$case_" in
hello)
    expect 'Hello, world!\n' < /dev/null
    # The same program as text, and as a packed executable.
    "$malm" asm --text "$dir/hello.mua" -o "$tmp/hello.mal" || exit 1
    test "$("$malm" run "$tmp/hello.mal")" = "Hello, world!" || exit 1
    "$malm" pack -o "$tmp/hello" "$tmp/p.malm" 2> /dev/null || exit 1
    test "$("$tmp/hello" < /dev/null)" = "Hello, world!" || exit 1
    ;;
cat)
    # Endless: the input, then byte 168 (0250) forever; head closes the pipe.
    printf 'hi\n' | "$malm" run "$tmp/p.malm" 2> /dev/null | head -c 5 > "$tmp/out"
    printf 'hi\n\250\250' | cmp - "$tmp/out" || exit 1
    ;;
halting_cat)
    printf 'Hello, cat!\n' | expect 'Hello, cat!\n'
    expect '' < /dev/null
    # All 256 byte values, binary-safe, through a packed executable too.
    i=0
    while [ $i -lt 256 ]; do
        printf "\\$(printf %o $i)"
        i=$((i + 1))
    done > "$tmp/all.bin"
    "$malm" run "$tmp/p.malm" < "$tmp/all.bin" | cmp - "$tmp/all.bin" || exit 1
    "$malm" pack -o "$tmp/hcat" "$tmp/p.malm" 2> /dev/null || exit 1
    "$tmp/hcat" < "$tmp/all.bin" | cmp - "$tmp/all.bin" || exit 1
    ;;
digital_root)
    echo 9875 | expect '2\n'
    printf '123456789' | expect '9\n'
    echo 0 | expect '0\n'
    ;;
adder)
    echo '123 456' | expect '579\n'
    echo '999+999' | expect '1998\n'
    echo '000 000' | expect '0\n'
    ;;
*)
    echo "unknown case $case_" >&2
    exit 2
    ;;
esac
