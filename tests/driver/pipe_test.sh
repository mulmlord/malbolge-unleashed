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
pack_cat)
    # printf 'hi' | packed cat: identical to `malm run` (cat_nostop: input, then 0xA8 forever; head
    # closes the pipe).
    "$malm" pack -o "$tmp/packed_cat" "$dir/cat_nostop.mal" 2> /dev/null || exit 1
    printf 'hi' | "$tmp/packed_cat" 2> /dev/null | head -c 5 > "$tmp/packed.out"
    printf 'hi' | "$malm" run "$dir/cat_nostop.mal" 2> /dev/null | head -c 5 > "$tmp/run.out"
    printf 'hi\250\250\250' | cmp - "$tmp/packed.out" || exit 1
    cmp "$tmp/run.out" "$tmp/packed.out" || exit 1
    ;;
pack_classic_cat)
    # M0 exit criterion: a packed classic program with piped stdin.
    "$malm" pack --classic -o "$tmp/packed_cat" "$dir/cat_nostop.mal" 2> /dev/null || exit 1
    "$tmp/packed_cat" < "$dir/cat5_input.bin" 2> /dev/null | head -c 7 > "$tmp/out"
    { cat "$dir/cat5_expected.bin"; printf '\250\250'; } | cmp - "$tmp/out" || exit 1
    ;;
pack_hello)
    "$malm" pack -o "$tmp/packed_hello" "$dir/hello.mal" 2> /dev/null || exit 1
    "$tmp/packed_hello" < /dev/null > "$tmp/out"
    test $? = 0 || exit 1
    test "$(cat "$tmp/out")" = "Hello, world." || exit 1
    "$malm" run "$dir/hello.mal" | cmp - "$tmp/out" || exit 1
    ;;
pack_beer99)
    "$malm" pack --classic -o "$tmp/packed_beer" "$dir/beer99.mal" 2> /dev/null || exit 1
    ( "$tmp/packed_beer" < /dev/null; echo $? > "$tmp/status" ) | cmp - "$dir/beer99.expected" \
        || exit 1
    test "$(cat "$tmp/status")" = 0 || exit 1
    ;;
pack_exit_a)
    # One-byte cat that halts; with --exit-a the status is A mod 256 ('A' leaves 25 in A).
    printf "u'a%%@\"K" > "$tmp/one.mal"
    "$malm" pack --exit-a -o "$tmp/packed_one" "$tmp/one.mal" 2> /dev/null || exit 1
    printf 'A' | "$tmp/packed_one" > "$tmp/out"
    status=$?
    test "$status" = 25 || { echo "unexpected exit status $status" >&2; exit 1; }
    test "$(cat "$tmp/out")" = A || exit 1
    "$malm" pack -o "$tmp/packed_plain" "$tmp/one.mal" 2> /dev/null || exit 1
    printf 'A' | "$tmp/packed_plain" > /dev/null || exit 1
    ;;
pack_ignores_args)
    "$malm" pack -o "$tmp/packed_hello" "$dir/hello.mal" 2> /dev/null || exit 1
    out=$("$tmp/packed_hello" --help foo run -o x < /dev/null) || exit 1
    test "$out" = "Hello, world." || exit 1
    ;;
pack_repro)
    "$malm" pack -o "$tmp/a" "$dir/hello.mal" 2> /dev/null || exit 1
    "$malm" pack -o "$tmp/b" "$dir/hello.mal" 2> /dev/null || exit 1
    cmp "$tmp/a" "$tmp/b" || exit 1
    # the same program as .malm and as text packs identically
    "$malm" export --malm "$dir/hello.mal" -o "$tmp/h.malm" || exit 1
    "$malm" pack -o "$tmp/c" "$tmp/h.malm" 2> /dev/null || exit 1
    cmp "$tmp/a" "$tmp/c" || exit 1
    ;;
pack_corrupt)
    "$malm" pack -o "$tmp/good" "$dir/hello.mal" 2> /dev/null || exit 1
    size=$(wc -c < "$tmp/good")
    # a byte lost from the middle of the payload: trailer and file size disagree
    { head -c $((size - 40)) "$tmp/good"; tail -c 39 "$tmp/good"; } > "$tmp/short"
    chmod +x "$tmp/short"
    "$tmp/short" < /dev/null > "$tmp/out" 2> "$tmp/err"
    test $? = 1 || exit 1
    grep -q 'corrupt packed executable' "$tmp/err" || exit 1
    test ! -s "$tmp/out" || exit 1
    # the offset field edited (byte 8 of the trailer: offset's low byte)
    cp "$tmp/good" "$tmp/badoff"
    printf '\377' | dd of="$tmp/badoff" bs=1 seek=$((size - 16)) conv=notrunc 2> /dev/null
    "$tmp/badoff" < /dev/null > /dev/null 2> "$tmp/err"
    test $? = 1 || exit 1
    grep -q 'corrupt packed executable' "$tmp/err" || exit 1
    ;;
pack_epipe)
    # E_IO (exit 1), not death by SIGPIPE, from a packed endless cat.
    "$malm" pack -o "$tmp/packed_cat" "$dir/cat_nostop.mal" 2> /dev/null || exit 1
    ( env --default-signal=PIPE "$tmp/packed_cat" < /dev/null 2> "$tmp/err"
      echo $? > "$tmp/status" ) | head -c 1 > /dev/null
    test "$(cat "$tmp/status")" = 1 || exit 1
    grep -q 'E_IO' "$tmp/err" || exit 1
    ;;
pack_replace)
    # -o over an existing file replaces it; failures leave no temp file behind.
    echo old > "$tmp/x"
    "$malm" pack -o "$tmp/x" "$dir/hello.mal" 2> /dev/null || exit 1
    test "$("$tmp/x" < /dev/null)" = "Hello, world." || exit 1
    "$malm" pack -o "$tmp/nodir/x" "$dir/hello.mal" 2> /dev/null && exit 1
    test "$(ls -A "$tmp" | wc -l)" = 1 || exit 1
    ;;
pack_closed_stdin)
    # Closed stdin must not hand the executable's own descriptor to the program as fd 0: same
    # E_IO (exit 1) as `malm run`, and no ELF bytes on stdout.
    "$malm" pack -o "$tmp/packed_cat" "$dir/cat5.mal" 2> /dev/null || exit 1
    "$tmp/packed_cat" <&- > "$tmp/out" 2> "$tmp/err"
    status=$?
    "$malm" run "$dir/cat5.mal" <&- > "$tmp/run.out" 2> "$tmp/run.err"
    run_status=$?
    test "$status" = 1 || { echo "unexpected exit status $status" >&2; exit 1; }
    test "$run_status" = 1 || exit 1
    grep -q 'E_IO' "$tmp/err" || exit 1
    cmp "$tmp/err" "$tmp/run.err" || exit 1
    cmp "$tmp/out" "$tmp/run.out" || exit 1
    test ! -s "$tmp/out" || exit 1
    ;;
pack_closed_stdout)
    # Output into a closed stdout: E_IO, exit 1, as `malm run`.
    "$malm" pack -o "$tmp/packed_hello" "$dir/hello.mal" 2> /dev/null || exit 1
    "$tmp/packed_hello" < /dev/null >&- 2> "$tmp/err"
    status=$?
    "$malm" run "$dir/hello.mal" < /dev/null >&- 2> "$tmp/run.err"
    run_status=$?
    test "$status" = 1 || { echo "unexpected exit status $status" >&2; exit 1; }
    test "$run_status" = 1 || exit 1
    grep -q 'E_IO' "$tmp/err" || exit 1
    cmp "$tmp/err" "$tmp/run.err" || exit 1
    ;;
pack_efbig)
    # A file-size limit gives EFBIG (not death by SIGXFSZ) and leaves no half-written file or temp.
    mkdir "$tmp/o"
    ( ulimit -f 16 && "$malm" pack -o "$tmp/o/x" "$dir/hello.mal" 2> "$tmp/err" )
    test $? = 1 || { echo "pack under a file-size limit must exit 1" >&2; exit 1; }
    grep -q 'cannot write' "$tmp/err" || exit 1
    test -z "$(ls -A "$tmp/o")" || { echo "left files behind" >&2; exit 1; }
    ;;
*)
    echo "unknown case $case_" >&2
    exit 2
    ;;
esac
