#!/bin/sh
# Real-pipe tests for `malm run`. Usage: pipe_test.sh <malm> <programs-dir> <case>
set -u
malm=$1
dir=$2
case_=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

case "$case_" in
halting_cat)
    # Halting 5-byte cat, byte-exact including 0x00 and 0xFF; the exit status must be 0.
    ( "$malm" run "$dir/cat5.mal" < "$dir/cat5_input.bin"; echo $? > "$tmp/status" ) \
        | cmp - "$dir/cat5_expected.bin" || exit 1
    test "$(cat "$tmp/status")" = 0 || exit 1
    ;;
loop_cat)
    # General (non-halting) cat loop: the input comes first, then 0xA8 forever; head closes the
    # pipe after 5 bytes.
    "$malm" run "$dir/cat_nostop.mal" < "$dir/cat5_input.bin" 2> /dev/null \
        | head -c 5 | cmp - "$dir/cat5_expected.bin" || exit 1
    ;;
epipe)
    # Endless output into a closed pipe: E_IO (exit 1), never death by signal. SIGPIPE is reset
    # to its default first, so a missing SIG_IGN in malm is caught even if the parent ignores it.
    ( env --default-signal=PIPE "$malm" run "$dir/cat_nostop.mal" < /dev/null 2> "$tmp/err"
      echo $? > "$tmp/status" ) | head -c 1 > /dev/null
    status=$(cat "$tmp/status")
    test "$status" = 1 || { echo "unexpected exit status $status" >&2; exit 1; }
    grep -q 'E_IO' "$tmp/err" || { echo "no E_IO diagnostic" >&2; exit 1; }
    ;;
hello)
    out=$("$malm" run "$dir/hello.mal") || exit 1
    test "$out" = "Hello, world." || exit 1
    ;;
*)
    echo "unknown case $case_" >&2
    exit 2
    ;;
esac
