#!/bin/sh
# Real-pipe tests for `malm rvref` on the clang-built test programs, bare and on the runtime, with
# the minimal C library or with picolibc (the programs named picolibc_*), the C++ programs with
# libc++ (cxx_*), also with exceptions and RTTI (cxx_eh_*), and the Rust and Zig programs (rust_*,
# zig_*).
# Usage: rvref_pipe_test.sh <malm> <elf-dir> <programs-dir> <case>
#            [minimal|picolibc|cxx|cxx_eh|rust|zig]
set -u
malm=$1
elf=$2
programs=$3
case_=$4
libc=${5:-minimal}
p=
if [ "$libc" != minimal ]; then
    p=${libc}_
fi
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
    out=$("$malm" rvref "$elf/${p}example_hello.elf") || exit 1
    test "$out" = "Hello, world!" || exit 1
    ;;
example_cat)
    # Binary-safe, also across many lines of line-buffered output.
    # Also input whose last line has no newline.
    printf 'line\nno newline' > "$tmp/in"
    for f in "$programs/cat5_input.bin" "$programs/beer99.expected" "$tmp/in" /dev/null; do
        cat "$f" | "$malm" rvref "$elf/${p}example_cat.elf" > "$tmp/out" || exit 1
        cmp "$tmp/out" "$f" || exit 1
    done
    ;;
fizzbuzz)
    # The exit status is main's return value: 0, or 2 with a message on stderr (which rvref
    # merges into stdout) for bad input.
    seq 1 100 | awk '{ if ($1 % 15 == 0) print "FizzBuzz"; else if ($1 % 3 == 0) print "Fizz";
        else if ($1 % 5 == 0) print "Buzz"; else print }' > "$tmp/want"
    echo 100 | "$malm" rvref "$elf/${p}example_fizzbuzz.elf" > "$tmp/out" || exit 1
    cmp "$tmp/out" "$tmp/want" || exit 1
    printf 15 | "$malm" rvref "$elf/${p}example_fizzbuzz.elf" > "$tmp/out" || exit 1
    head -n 15 "$tmp/want" | cmp "$tmp/out" - || exit 1
    for input in nope 0 15x ""; do
        status=0
        printf '%s\n' "$input" | "$malm" rvref "$elf/${p}example_fizzbuzz.elf" > "$tmp/out" ||
            status=$?
        test "$status" = 2 || exit 1
        test "$(cat "$tmp/out")" = "fizzbuzz: expected a number from 1 to 10000" || exit 1
    done
    ;;
wc)
    # The counts of `wc` (POSIX format: no padding), on text, binary input and nothing.
    printf 'one two\n  three\t\nfour' > "$tmp/in"
    for f in "$tmp/in" "$programs/beer99.expected" "$programs/cat5_input.bin" /dev/null; do
        "$malm" rvref "$elf/${p}example_wc.elf" < "$f" > "$tmp/out" || exit 1
        wc < "$f" | awk '{ print $1, $2, $3 }' | cmp "$tmp/out" - || exit 1
    done
    ;;
sort)
    # Bytewise like `LC_ALL=C sort`: empty lines, a NUL byte, bytes above 127, duplicates and a
    # last line without a newline.
    printf 'pear\nApple\n\napple\n\303\244\nx\000y\npear\nfig' > "$tmp/in"
    for f in "$tmp/in" "$programs/beer99.expected" /dev/null; do
        "$malm" rvref "$elf/${p}example_sort.elf" < "$f" > "$tmp/out" || exit 1
        LC_ALL=C sort < "$f" | cmp "$tmp/out" - || exit 1
    done
    ;;
runtime)
    # The self-check: stdout and stderr in order, the atexit handlers after the summary, and
    # exit(42) from a nested call.
    status=0
    printf 'line1\nxyz' | "$malm" rvref "$elf/${p}runtime_test.elf" > "$tmp/out" || status=$?
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
    echo assert | "$malm" rvref "$elf/${p}runtime_test.elf" > "$tmp/out" || status=$?
    test "$status" = 134 || exit 1
    if [ "$libc" = picolibc ]; then
        want="^assertion \"c == 'b'\" failed: file \".*runtime_test\.c\", line [0-9]*,"
        want="$want function: main$"
    else
        want="runtime_test\.c:[0-9]*: main: Assertion \`c == 'b'' failed\.$"
    fi
    tail -n 1 "$tmp/out" | grep -q "$want" || exit 1
    ;;
self_check)
    # libc++'s self-check: the summary, then the static destructors in reverse order, and main's
    # 42.
    status=0
    "$malm" rvref "$elf/${p}test.elf" < /dev/null > "$tmp/out" || status=$?
    if [ "$status" != 42 ]; then
        cat "$tmp/out" >&2
        exit 1
    fi
    sed -n 1p "$tmp/out" | grep -Eq '^cxx_test: [0-9]+ checks passed$' || exit 1
    test "$(sed -n '2,$p' "$tmp/out")" = "$(printf '~Global 2\n~Global 1')" || exit 1
    ;;
abort)
    # std::vector::at out of range: libc++'s message, or with exceptions libc++abi's for the
    # uncaught std::out_of_range (its type mangled), then abort's status 134.
    status=0
    echo abort | "$malm" rvref "$elf/${p}test.elf" > "$tmp/out" || status=$?
    test "$status" = 134 || exit 1
    if [ "$libc" = cxx_eh ]; then
        want='libc++abi: terminating due to uncaught exception of type St12out_of_range: vector'
    else
        want='out_of_range was thrown in -fno-exceptions mode with message "vector"'
    fi
    test "$(cat "$tmp/out")" = "$want" || exit 1
    ;;
checks)
    # Rust and Zig: the runtime's self-check, ending with main's 42.
    status=0
    "$malm" rvref "$elf/${p}test.elf" < /dev/null > "$tmp/out" || status=$?
    if [ "$status" != 42 ]; then
        cat "$tmp/out" >&2
        exit 1
    fi
    grep -Eqx "${p}test: [0-9]+ checks passed" "$tmp/out" || exit 1
    test "$(wc -l < "$tmp/out")" = 1 || exit 1
    ;;
panic)
    # Rust and Zig: what stdout has buffered, then the panic: for Rust its location and message
    # and status 101, for Zig its message and abort's 134.
    status=0
    echo panic | "$malm" rvref "$elf/${p}test.elf" > "$tmp/out" || status=$?
    sed -n 1p "$tmp/out" | grep -qx 'before the panic' || exit 1
    if [ "$libc" = zig ]; then
        test "$status" = 134 || exit 1
        test "$(sed -n '2,$p' "$tmp/out")" = 'panic: index out of bounds: index 5, len 3' || exit 1
    else
        test "$status" = 101 || exit 1
        sed -n 2p "$tmp/out" | grep -Eq '^panicked at .*rust_test\.rs:[0-9]+:[0-9]+:$' || exit 1
        test "$(sed -n '3,$p' "$tmp/out")" = \
            'index out of bounds: the len is 0 but the index is 3' || exit 1
    fi
    ;;
error)
    # Rust and Zig: main returns an error: on stderr its Debug form (Rust) or name (Zig), status 1.
    status=0
    echo error | "$malm" rvref "$elf/${p}test.elf" > "$tmp/out" || status=$?
    want='Error: "asked for it"'
    if [ "$libc" = zig ]; then
        want='error: AskedForIt'
    fi
    test "$status" = 1 && test "$(cat "$tmp/out")" = "$want" || exit 1
    ;;
exit)
    # Rust and Zig: process::exit (malm.exit) writes what stdout has buffered (no newline), then
    # ends with its status.
    status=0
    echo exit | "$malm" rvref "$elf/${p}test.elf" > "$tmp/out" || status=$?
    test "$status" = 7 && test "$(cat "$tmp/out")" = 'buffered until exit' || exit 1
    ;;
stats)
    # picolibc only: floating point, scanf and sqrt.
    printf '2 4 4 4 5 5 7 9' | "$malm" rvref "$elf/${p}example_stats.elf" > "$tmp/out" || exit 1
    test "$(cat "$tmp/out")" = "$(printf 'count 8\nmin 2\nmax 9\nmean 5\nstddev 2')" || exit 1
    printf '1.5 -2.25\n1e3\n' | "$malm" rvref "$elf/${p}example_stats.elf" > "$tmp/out" || exit 1
    test "$(cat "$tmp/out")" = \
        "$(printf 'count 3\nmin -2.25\nmax 1000\nmean 333.083\nstddev 471.584')" || exit 1
    "$malm" rvref "$elf/${p}example_stats.elf" < /dev/null > "$tmp/out" || exit 1
    test "$(cat "$tmp/out")" = "count 0" || exit 1
    status=0
    echo '3 x' | "$malm" rvref "$elf/${p}example_stats.elf" > "$tmp/out" || status=$?
    test "$status" = 2 && test "$(cat "$tmp/out")" = "stats: expected numbers" || exit 1
    ;;
*)
    echo "unknown case $case_" >&2
    exit 2
    ;;
esac
