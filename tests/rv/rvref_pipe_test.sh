#!/bin/sh
# Real-pipe tests for `malm rvref` on the clang-built test programs, bare and on the runtime.
# Usage: rvref_pipe_test.sh <malm> <elf-dir> <programs-dir> <case>
set -u
malm=$1
elf=$2
programs=$3
case_=$4
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

case "$case_" in
hello)
    out=$("$malm" rvref "$elf/hello.elf") || exit 1
    test "$out" = "Hello, world!" || exit 1
    ;;
cat)
    # Binary-safe copy of stdin; the exit status is the byte count mod 256.
    size=$(wc -c < "$programs/cat5_input.bin")
    ( "$malm" rvref "$elf/cat.elf" < "$programs/cat5_input.bin"; echo $? > "$tmp/status" ) \
        | cmp - "$programs/cat5_input.bin" || exit 1
    test "$(cat "$tmp/status")" = $((size % 256)) || exit 1
    "$malm" rvref "$elf/cat.elf" < /dev/null > "$tmp/out" || exit 1
    test ! -s "$tmp/out" || exit 1
    ;;
trace)
    # stdout is unchanged; stderr has one line per instruction, ending with the exit ecall.
    out=$("$malm" rvref --trace "$elf/hello.elf" 2> "$tmp/err") || exit 1
    test "$out" = "Hello, world!" || exit 1
    test "$(head -n 1 "$tmp/err" | cut -d' ' -f1-2)" = "step=1 pc=0x00001000" || exit 1
    tail -n 1 "$tmp/err" | grep -q ' insn=0x00000073$' || exit 1
    test "$(grep -vc '^step=' "$tmp/err")" = 0 || exit 1
    ;;
regs)
    # sp starts at the top of guest RAM, so --ram shows up in the final registers.
    "$malm" rvref --regs --ram 64K "$elf/hello.elf" > /dev/null 2> "$tmp/err" || exit 1
    grep -q '^x2 sp    0x00010000$' "$tmp/err" || exit 1
    grep -q '^x17 a7   0x0000005d$' "$tmp/err" || exit 1
    test "$(grep -c '^x[0-9]' "$tmp/err")" = 32 || exit 1
    ;;
example_hello)
    out=$("$malm" rvref "$elf/example_hello.elf") || exit 1
    test "$out" = "Hello, world!" || exit 1
    ;;
example_cat)
    # Binary-safe, also across many lines of line-buffered output.
    # Also input whose last line has no newline.
    printf 'line\nno newline' > "$tmp/in"
    for f in "$programs/cat5_input.bin" "$programs/beer99.expected" "$tmp/in" /dev/null; do
        cat "$f" | "$malm" rvref "$elf/example_cat.elf" > "$tmp/out" || exit 1
        cmp "$tmp/out" "$f" || exit 1
    done
    ;;
fizzbuzz)
    # The exit status is main's return value: 0, or 2 with a message on stderr (which rvref
    # merges into stdout) for bad input.
    seq 1 100 | awk '{ if ($1 % 15 == 0) print "FizzBuzz"; else if ($1 % 3 == 0) print "Fizz";
        else if ($1 % 5 == 0) print "Buzz"; else print }' > "$tmp/want"
    echo 100 | "$malm" rvref "$elf/example_fizzbuzz.elf" > "$tmp/out" || exit 1
    cmp "$tmp/out" "$tmp/want" || exit 1
    printf 15 | "$malm" rvref "$elf/example_fizzbuzz.elf" > "$tmp/out" || exit 1
    head -n 15 "$tmp/want" | cmp "$tmp/out" - || exit 1
    for input in nope 0 15x ""; do
        status=0
        printf '%s\n' "$input" | "$malm" rvref "$elf/example_fizzbuzz.elf" > "$tmp/out" ||
            status=$?
        test "$status" = 2 || exit 1
        test "$(cat "$tmp/out")" = "fizzbuzz: expected a number from 1 to 10000" || exit 1
    done
    ;;
runtime)
    # The self-check: stdout and stderr in order, the atexit handlers after the summary, and
    # exit(42) from a nested call.
    status=0
    printf 'line1\nxyz' | "$malm" rvref "$elf/runtime_test.elf" > "$tmp/out" || status=$?
    if [ "$status" != 42 ]; then
        cat "$tmp/out" >&2
        exit 1
    fi
    test "$(sed -n 1,2p "$tmp/out")" = "$(printf 'ab\nc')" || exit 1
    sed -n 3p "$tmp/out" | grep -Eq '^runtime_test: [0-9]+ checks passed$' || exit 1
    test "$(sed -n '4,$p' "$tmp/out")" = "$(printf 'atexit 2\natexit 1')" || exit 1
    test "$(tail -c 1 "$tmp/out")" = 1 || exit 1
    ;;
runtime_assert)
    # A failed assert: the message on stderr, then abort's status 134.
    status=0
    echo assert | "$malm" rvref "$elf/runtime_test.elf" > "$tmp/out" || status=$?
    test "$status" = 134 || exit 1
    tail -n 1 "$tmp/out" | grep -q "runtime_test\.c:[0-9]*: main: Assertion \`c == 'b'' failed\.$" ||
        exit 1
    ;;
*)
    echo "unknown case $case_" >&2
    exit 2
    ;;
esac
