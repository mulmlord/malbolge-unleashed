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
    # General (non-halting) cat loop: the input comes first, then 0xA8 (0250) forever; head closes
    # the pipe after 7 bytes.
    "$malm" run "$dir/cat_nostop.mal" < "$dir/cat5_input.bin" 2> /dev/null \
        | head -c 7 > "$tmp/out"
    { cat "$dir/cat5_expected.bin"; printf '\250\250'; } | cmp - "$tmp/out" || exit 1
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
classic_beer99)
    # 99 bottles in classic mode: the full song, byte-exact, exit status 0.
    ( "$malm" run --classic "$dir/beer99.mal" < /dev/null; echo $? > "$tmp/status" ) \
        | cmp - "$dir/beer99.expected" || exit 1
    test "$(cat "$tmp/status")" = 0 || exit 1
    ;;
classic_loop_cat)
    # The non-halting cat in classic mode: input, then byte 168 (0250; EOF 59048 mod 256) forever.
    "$malm" run --classic "$dir/cat_nostop.mal" < "$dir/cat5_input.bin" 2> /dev/null \
        | head -c 7 > "$tmp/out"
    { cat "$dir/cat5_expected.bin"; printf '\250\250'; } | cmp - "$tmp/out" || exit 1
    ;;
malm_hello)
    # A .malm run through a real pipe and the same program as text give identical output.
    "$malm" export --malm "$dir/hello.mal" -o "$tmp/hello.malm" || exit 1
    out=$("$malm" run "$tmp/hello.malm") || exit 1
    test "$out" = "Hello, world." || exit 1
    ;;
malm_beer99)
    # Classic .malm: the header's mode applies without --classic; exit status 0.
    "$malm" export --malm --classic "$dir/beer99.mal" -o "$tmp/beer99.malm" || exit 1
    ( "$malm" run "$tmp/beer99.malm" < /dev/null; echo $? > "$tmp/status" ) \
        | cmp - "$dir/beer99.expected" || exit 1
    test "$(cat "$tmp/status")" = 0 || exit 1
    ;;
malm_loop_cat)
    # Non-halting cat from a .malm, closed by head; also the export -o - pipe into a file.
    "$malm" export --malm "$dir/cat_nostop.mal" -o - > "$tmp/cat.malm" || exit 1
    "$malm" run "$tmp/cat.malm" < "$dir/cat5_input.bin" 2> /dev/null | head -c 7 > "$tmp/out"
    { cat "$dir/cat5_expected.bin"; printf '\250\250'; } | cmp - "$tmp/out" || exit 1
    ;;
malm_epipe)
    # E_IO (exit 1, no signal death) also when the program comes from a .malm.
    "$malm" export --malm "$dir/cat_nostop.mal" -o "$tmp/cat.malm" || exit 1
    ( env --default-signal=PIPE "$malm" run "$tmp/cat.malm" < /dev/null 2> "$tmp/err"
      echo $? > "$tmp/status" ) | head -c 1 > /dev/null
    test "$(cat "$tmp/status")" = 1 || exit 1
    grep -q 'E_IO' "$tmp/err" || exit 1
    ;;
malm_text_pipe)
    # export --text to a pipe and back: byte-identical to the original (whitespace stripped).
    "$malm" export --malm "$dir/beer99.mal" -o "$tmp/b.malm" || exit 1
    "$malm" export --text "$tmp/b.malm" -o - | tr -d ' \t\n\r' > "$tmp/b.txt" || exit 1
    tr -d ' \t\n\r' < "$dir/beer99.mal" | cmp - "$tmp/b.txt" || exit 1
    ;;
*)
    echo "unknown case $case_" >&2
    exit 2
    ;;
esac
