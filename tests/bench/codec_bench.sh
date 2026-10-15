#!/bin/sh
# Compression benchmark (issue #39, docs/benchmarks.md): builds the benchmark programs, compresses
# each program's normalized cells in several layouts, levels and windows (codec_bench) and with xz
# (if installed), then times loading the program's .malm with each codec. Prints two Markdown
# tables.
#
# Usage: codec_bench.sh <malm> <codec_bench> [<baseline codec_bench>]
#
# With a baseline (codec_bench built from an older commit), the load table has a column for it, on
# the codec-1 file. Compression is single-threaded throughout, except the row "as written", which
# uses 8 workers as `malm` does.
set -eu

malm=$1
bench=$2
base=${3:-}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# name|build command writing "$out"
cat > "$tmp/programs" <<'EOF'
asm adder|"$malm" asm "$root/examples/asm/adder.mua" -o "$out"
hla rev|"$malm" hla "$root/examples/hla/rev.mhl" -o "$out"
C hello, emu, 1M|"$malm" build "$root/examples/c/hello.c" -o "$out"
C fizzbuzz, emu, 1M|"$malm" build "$root/examples/c/fizzbuzz.c" -o "$out"
C hello, aot, 1M|"$malm" build --mode aot "$root/examples/c/hello.c" -o "$out"
C hello, emu, 4M|"$malm" build --ram 4M "$root/examples/c/hello.c" -o "$out"
EOF

# label|codec_bench options
cat > "$tmp/configs" <<'EOF'
packed, zstd 9 (codec 1)|--layout p3 --level 9
packed, zstd 19|--layout p3 --level 19
2 cells per byte, zstd 12|--layout s6 --level 12
2 cells per byte, zstd 19|--layout s6 --level 19
1 cell per byte, zstd 12|--layout s8 --level 12
1 cell per byte, zstd 9, long, 2^27|--layout s8 --level 9 --long --window-log 27
1 cell per byte, zstd 19 (codec 2)|--layout s8 --level 19
1 cell per byte, zstd 19, as written|--layout s8 --level 19 --workers 8
1 cell per byte, zstd 19, 2^27|--layout s8 --level 19 --window-log 27
EOF

now() { date +%s.%N; }
secs() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2f", b - a }'; }

echo "| program | compression | bytes | of packed | compress | decompress |"
echo "|---|---|---|---|---|---|"
: > "$tmp/load"
while IFS='|' read -r name cmd; do
    out="$tmp/p.malm"
    rm -f "$out"
    eval "$cmd" 2> /dev/null
    "$malm" export --malm --codec packed -o "$tmp/packed.malm" "$out"
    packed=$(($(wc -c < "$tmp/packed.malm") - 32))
    while IFS='|' read -r label opts; do
        # shellcheck disable=SC2086
        set -- $("$bench" $opts "$out")
        awk -v name="$name" -v label="$label" -v z="$2" -v p="$packed" -v c="$3" -v d="$4" \
            'BEGIN { printf "| %s | %s | %d | %.1f%% | %.2f s | %.3f s |\n",
                     name, label, z, 100 * z / p, c, d }'
    done < "$tmp/configs"
    if command -v xz > /dev/null; then
        for layout in p3 s8; do
            "$bench" --layout $layout --dump "$tmp/raw" "$out"
            t0=$(now)
            xz -9e -T1 -c "$tmp/raw" > "$tmp/raw.xz"
            t1=$(now)
            xz -dc "$tmp/raw.xz" > /dev/null
            t2=$(now)
            label="packed, xz -9e"
            [ $layout = s8 ] && label="1 cell per byte, xz -9e"
            z=$(wc -c < "$tmp/raw.xz")
            awk -v name="$name" -v label="$label" -v z="$z" -v p="$packed" \
                -v c="$(secs "$t0" "$t1")" -v d="$(secs "$t1" "$t2")" \
                'BEGIN { printf "| %s | %s | %d | %.1f%% | %.2f s | %.3f s |\n",
                         name, label, z, 100 * z / p, c, d }'
        done
    fi

    "$malm" export --malm --codec zstd-packed -o "$tmp/c1.malm" "$out"
    "$malm" export --malm -o "$tmp/c2.malm" "$out"
    old=
    [ -z "$base" ] || old=$("$base" --load "$tmp/c1.malm")
    echo "$name|$(wc -c < "$tmp/c1.malm")|$(wc -c < "$tmp/c2.malm")|$old|$("$bench" --load \
        "$tmp/c1.malm")|$("$bench" --load "$tmp/c2.malm")" >> "$tmp/load"
done < "$tmp/programs"

echo
if [ -n "$base" ]; then
    echo "| program | cells | codec 1 | codec 2 | read before (codec 1) | read, codec 1 | read, codec 2 | memory |"
    echo "|---|---|---|---|---|---|---|---|"
else
    echo "| program | cells | codec 1 | codec 2 | read, codec 1 | read, codec 2 | memory |"
    echo "|---|---|---|---|---|---|---|"
fi
awk -F'|' -v cmp="$base" '{
    split($4, o, " "); split($5, a, " "); split($6, b, " ")
    before = cmp == "" ? "" : sprintf(" %.3f s |", o[3])
    printf "| %s | %d | %d B | %d B |%s %.3f s | %.3f s | %.3f s |\n", $1, b[2], $2, $3, before, a[3],
           b[3], b[4]
}' "$tmp/load"
