# THROWAWAY prototype code for the spike #18 (docs/adr/0001-hla-word-representation.md).
#
# Assembles a program from gen.py, runs it with --trace and reports, per operation, the steps
# between its two Out markers (the call, the routine and the return), and checks the results read
# back after each operation against gen.py's expectations.
#
# Usage: python3 measure.py L [malm]

import json
import os
import re
import statistics
import subprocess
import sys

from tern import TRITS, V

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")


def parse_value(s):
    """Spec 1.1 notation as printed by --trace: 0t... or ...h... ."""
    if s.startswith("0t"):
        return V.nat(int(s[2:], 3))
    assert s.startswith("..."), s
    digits = s[3:]
    head, low = int(digits[0]), [int(c) for c in reversed(digits[1:])]
    return V.make(head, low)


def run(L, malm):
    os.makedirs(OUT, exist_ok=True)
    mua = os.path.join(OUT, f"bench_L{L}.mua")
    subprocess.run([sys.executable, os.path.join(HERE, "gen.py"), str(L), mua], check=True)
    meta = json.load(open(mua[:-4] + ".json"))
    txt = mua[:-4] + ".txt"
    subprocess.run([malm, "asm", "--text", "-o", txt, mua], check=True)
    size = os.path.getsize(txt)
    malmf = mua[:-4] + ".malm"
    mapf = mua[:-4] + ".map"
    subprocess.run([malm, "asm", "-o", malmf, "--map", mapf, mua], check=True)
    # Return flags (one per call site; FC, the carry, is part of the add algorithm).
    ret_flags = set()
    for line in open(mapf):
        name, addr = line.split()[:2]
        if re.fullmatch(r"F\d+|FJ\d", name):
            ret_flags.add(addr)
    trace = subprocess.run([malm, "run", "--trace", "--stats", malmf], stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True, stdin=subprocess.DEVNULL)
    outs = []
    stats = {}
    skips = 0
    for line in trace.stderr.splitlines():
        if " op=Nop " in line and re.search(r" C=(\S+)", line).group(1) in ret_flags:
            skips += 1
        elif " op=Out " in line:
            step = int(re.search(r"step=(\d+)", line).group(1))
            a = re.search(r" A=(\S+)", line).group(1)
            outs.append((step, parse_value(a), skips))
        elif line.startswith("  "):
            stats[line[:15].strip()] = line[15:].strip()
    return meta, outs, stats, size, os.path.getsize(malmf)


def decode(L, top, v):
    out = 0
    for lane in range(L):
        for i in range(8):
            t = v.t[4 + 8 * lane + i]
            assert t in (0, 2), v
            out |= (t // 2) << (8 * lane + i)
    return out


def main():
    L = int(sys.argv[1])
    malm = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        HERE, "../../build/release/src/driver/malm")
    meta, outs, stats, size, malm_size = run(L, malm)
    nc = meta["ncells"]
    pos = 0
    steps = {}
    ok = True
    for kind, expected in meta["expect"]:
        (s0, _, k0), (s1, _, k1) = outs[pos], outs[pos + 1]
        pos += 2
        steps.setdefault(kind, []).append((s1 - s0 - 1, s1 - s0 - 1 - 3 * (k1 - k0)))
        if isinstance(expected, dict):
            if outs[pos - 1][1].lit() != expected["a"]:
                ok = False
                print(f"MISMATCH {kind}: A = {outs[pos - 1][1].lit()} expected {expected['a']}")
        elif expected is not None:
            got = 0
            for c in range(nc):
                got |= decode(L, meta["top"], outs[pos][1]) << (8 * L * c)
                pos += 1
            if got != expected:
                ok = False
                print(f"MISMATCH {kind}: got {got:#010x} expected {expected:#010x}")
    assert pos == len(outs), (pos, len(outs))
    print(f"L={L} ({nc} cell(s) per word): results {'OK' if ok else 'WRONG'}")
    for kind, v in steps.items():
        a = [x for x, _ in v]
        b = [y for _, y in v]
        print(f"  {kind:7} steps {statistics.mean(a):8.1f} (min {min(a)}, max {max(a)}); without"
              f" skipped return flags {statistics.mean(b):8.1f} (min {min(b)}, max {max(b)})")
    first = outs[0][0]
    print(f"  startup steps (to the first marker) {first}, text {size} chars, .malm {malm_size} B,"
          f" table cells {meta['table_cells']}, n {stats.get('n')}, halted {stats.get('halted')}")


if __name__ == "__main__":
    main()
