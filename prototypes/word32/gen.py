# THROWAWAY prototype code for the spike #18 (docs/adr/0001-hla-word-representation.md).
#
# Generates a .mua benchmark program for one 32-bit word representation and the expected results.
#
# Representation: bit b is the trit 2*b ("f02"). A word is 4/L cells of L byte lanes each: L = 1
# is four byte cells, L = 2 two 16-bit halves, L = 4 one word cell. In a cell, lane l holds its
# bits at trits BO + 8l .. BO + 8l + 7; the other trits and the head are 0. The low trits 0..3 are
# free so that a table index can spell the lane and the carry-in there.
#
# Machinery (README.md has the measured costs):
#  * Register: a data block with entries for Opr (`r_O`), table dispatch (`r_M`) and RAM access
#    (`r_A`), the value cell, and a chain of return flags, one per call site (docs/asm.md 3.3).
#  * Operation: a subroutine on fixed registers X, Y, Z (scratch S, I, P), called from the main
#    stream and returning through its own flag chain, as an HLA ALU routine would.
#  * Sequences: SET (load a per-position constant into A by inline fixed-point Oprs) and OPR on a
#    register, found by search.py / mask_search.py and checked here by simulation.
#  * add: per cell, S := column sums of X and Y (one ternary digit per bit). Per lane, I := the
#    lane's sums with the lane number and the carry-in in the low trits; MovD through I into a
#    table of 2-cell entries; the entry continues in the stream of (lane, sum byte, carry-out),
#    which loads the merge constant for Z, sets the carry flag and returns.
#  * RAM: a record is [value][MB][BACK] at an address whose trits spell the address bits. A pointer
#    register P holds record - 2; the access block RA does MovD through P, an RNop and the Opr on
#    the value; [record+1] and [record+2] lead back to BACK, which restores RA and returns.
#    The rotate-n read uses RR (MovD, then Rot) with pointer record - 1 and BACKR instead.
#
# Usage: python3 gen.py L out.mua    (writes out.mua and out.json)

import json
import random
import sys

from tern import TRITS, V, C1, crz, load_seq

BO = 4
HEAD = TRITS - 1


class Layout:
    def __init__(self, L):
        self.L = L
        self.ncells = 4 // L
        self.top = BO + 8 * L  # trits >= top are like the head

    def lane_pos(self, lane):
        return range(BO + 8 * lane, BO + 8 * lane + 8)

    def encode(self, byte_values):
        t = [0] * TRITS
        for lane, b in enumerate(byte_values):
            for i, p in enumerate(self.lane_pos(lane)):
                t[p] = 2 * ((b >> i) & 1)
        return V(t)

    def decode(self, v):
        out = 0
        for lane in range(self.L):
            for i, p in enumerate(self.lane_pos(lane)):
                assert v.t[p] in (0, 2), v
                out |= (v.t[p] // 2) << (8 * lane + i)
        return out

    def word(self, w):
        bs = [(w >> (8 * k)) & 255 for k in range(4)]
        return [self.encode(bs[c * self.L:(c + 1) * self.L]) for c in range(self.ncells)]

    def classes(self, cls_of):
        """A value from a function position -> trit (positions >= top share the head's)."""
        t = [cls_of(min(p, self.top)) for p in range(TRITS)]
        return V(t)


# Sequences. SET takes a function position -> trit (the class constants applied to the layout).
def bits_head(ly, bits, head):
    return ly.classes(lambda p: bits if BO <= p < ly.top else head)


# search.py: xor, sum (sigma = (1, 2, 0)), reset1; classes (bits, head).
XOR = [("SET", (1, 1)), ("OPR", "Z"), ("OPR", "Z"), ("OPR", "X"), ("OPR", "Z"), ("SET", (1, 1)),
       ("OPR", "Y"), ("OPR", "Z"), ("SET", (1, 1)), ("OPR", "Z")]
SUM = [("SET", (2, 0)), ("OPR", "X"), ("OPR", "S"), ("SET", (1, 1)), ("OPR", "Y"), ("OPR", "S"),
       ("SET", (2, 1)), ("OPR", "X"), ("OPR", "S")]
SIGMA = (1, 2, 0)
RESET = [("SET", (1, 1)), ("OPR", "S"), ("OPR", "S")]
# Z := X, touching X once (search.py "mov"), for a load from RAM.
MOV1 = [("SET", (1, 1)), ("OPR", "Z"), ("OPR", "Z"), ("OPR", "X"), ("OPR", "Z"), ("SET", (1, 1)),
        ("OPR", "Z")]
# Hand-derived (README.md): Z := X touching Z three times and X twice, for a store to RAM.
MOV = [("SET", (1, 1)), ("OPR", "Z"), ("OPR", "Z"), ("SET", (0, 0)), ("OPR", "X"), ("OPR", "Z"),
       ("SET", (1, 1)), ("OPR", "X")]
# mask_search.py: classes (lane, [other,] fh, fa, fb).
MASK_BYTE = [("SET", (2, 1, 1, 2)), ("OPR", "S"), ("OPR", "I"), ("SET", (2, 0, 0, 2)), ("OPR", "S")]
MASK_WORD = [("SET", (2, 2, 0, 0, 0)), ("OPR", "S"), ("SET", (0, 1, 0, 0, 2)), ("OPR", "I"),
             ("SET", (2, 2, 0, 0, 0)), ("OPR", "S"), ("OPR", "I"), ("SET", (2, 0, 0, 2, 2)),
             ("OPR", "I")]


def simulate(seq, cells, set_value):
    a = None
    for op, arg in seq:
        if op == "SET":
            a = set_value(arg)
        else:
            cells[arg] = a = crz(a, cells[arg])
    return cells


class Asm:
    def __init__(self, width):
        self.width = width
        self.flags = []
        self.regs = {}
        self.blocks = []
        self.uid = 0

    def label(self, prefix):
        self.uid += 1
        return f"{prefix}{self.uid}"

    def flag(self, name=None):
        name = name or self.label("F")
        self.flags.append(name)
        return name

    def reg(self, name, init, dispatch=False, ram=False):
        self.regs[name] = {"init": init, "sites": [], "dispatch": dispatch, "ram": ram}

    def set(self, out, v):
        start, consts = load_seq(v)
        words = [f"ROT {'C1' if start.head == 1 else 'C0'} R_ROT"]
        words += [f"OPR {k.lit()} R_OPR" for k in consts]
        out.append(" ".join(words))

    def call(self, out, entry, sites, restore):
        f = self.flag()
        ret = self.label("RET")
        out.append(f"R_{f} MOVED {entry}")
        # A register entry leaves MOVED disarmed; a subroutine re-arms it at its entry label.
        out.append(f"{ret}: {restore}" if restore != "" else f"{ret}:")
        sites.append((f, ret))

    def opr(self, out, reg):
        self.call(out, f"{reg}_O", self.regs[reg]["sites"], "R_MOVED R_OPR")

    def seq(self, out, seq, regs, set_value):
        for op, arg in seq:
            if op == "SET":
                self.set(out, set_value(arg))
            elif regs[arg] in self.regs:
                self.opr(out, regs[arg])
            else:
                regs[arg](out)  # a RAM cell: a callable that emits the access

    def chain(self, sites):
        return [f"    {f} {ret} R_{f}" for f, ret in sites]

    def render(self, main):
        o = [f".WIDTH {self.width}", "", ".CODE"]
        for u in ("OUT: Out/Nop", "ROT: Rot/Nop", "OPR: Opr/Nop", "MOVED: MovD/Nop", "MD: MovD/Nop",
                  "MD2: MovD/Nop", "MB: MovD/Nop"):
            o += [u, "    Jmp", ""]
        o += ["RA: MovD/Nop/Nop/Nop RNop Opr/Nop/Nop/Nop", "    Jmp", ""]
        o += ["RR: MovD/Nop/Nop/Nop", "RR2: Rot/Nop/Nop/Nop/Nop/Nop", "    Jmp", "", "HALT: Hlt", ""]
        o += [f"{f}: Nop/MovD\n    Jmp\n" for f in self.flags]
        o += [".DATA", "{"] + [("ENTRY: " if i == 0 else "    ") + w for i, w in enumerate(main)]
        o += ["    HALT", "}", ""]
        for name, r in self.regs.items():
            o.append("{")
            if r["ram"] == "rot":
                o.append(f"{name}_A: U_RR {name}")
            elif r["ram"]:
                o.append(f"{name}_A: U_RA {name}")
            if r["dispatch"]:
                o.append(f"{name}_M: U_MD {name}")
            o.append(f"{name}_O: OPR {name}: {r['init'].lit()}")
            o += self.chain(r["sites"])
            o.append("}")
        return "\n".join(o + [b() if callable(b) else b for b in self.blocks]) + "\n"


class Bench:
    def __init__(self, L, seed=1):
        self.ly = Layout(L)
        self.L = L
        self.asm = Asm(max(self.ly.top + 2, 24))
        self.rng = random.Random(seed)
        self.main = []
        self.expect = []  # (kind, expected word), one per measured window
        self.subs = {}

    # Helpers on whole words (lists of cell registers).
    def regs(self, name, w=0, **kw):
        for c, v in enumerate(self.ly.word(w)):
            self.asm.reg(f"{name}{c}", v, **kw)
        return [f"{name}{c}" for c in range(self.ly.ncells)]

    def bh(self, arg):
        return bits_head(self.ly, *arg)

    def subroutine(self, name, body):
        """`body(out)` emits the routine; returns a function that emits a call into `out`."""
        sites = []
        out = [f"{name}: R_MOVED"]
        body(out)
        self.asm.blocks.append(lambda: "\n.DATA\n{\n" + "\n".join(
            ("    " if i else "") + w for i, w in enumerate(out)) + "\n" + "\n".join(
            self.asm.chain(sites)) + "\n}")

        def call(o):
            self.asm.call(o, name, sites, "")

        return call

    def measure(self, kind, call, result_regs, expected):
        m = self.main
        m.append("OUT ?- R_OUT")
        call(m)
        m.append("OUT ?- R_OUT")
        for r in result_regs:  # A := the register (Opr with C1 is the identity on f02 cells)
            self.asm.set(m, C1)
            self.asm.opr(m, r)
            m.append("OUT ?- R_OUT")
        self.expect.append((kind, expected))

    def measure_a(self, kind, call, expected):
        """Like measure, but the result is A at the closing marker (a raw value)."""
        self.main.append("OUT ?- R_OUT")
        call(self.main)
        self.main.append("OUT ?- R_OUT")
        self.expect.append((kind, {"a": expected.lit()}))

    def load_word(self, regs, w):
        """Untimed: write the constant w into the registers (reset, then one Opr)."""
        for r, v in zip(regs, self.ly.word(w)):
            self.asm.seq(self.main, RESET, {"S": r}, self.bh)
            self.asm.set(self.main, V([(1, 2)[x // 2] for x in v.t]))
            self.asm.opr(self.main, r)

    # Operations.
    def xor_routine(self, X, Y, Z):
        def body(out):
            for c in range(self.ly.ncells):
                self.asm.seq(out, XOR, {"X": X[c], "Y": Y[c], "Z": Z[c]}, self.bh)
        return self.subroutine("XOR", body)

    def combo(self, lane, cin):
        k = 2 * lane + cin
        return [(k >> i) & 1 for i in range(3)]

    def mask_value(self, lane, cin, arg):
        """The SET constant of the mask sequence for (lane, cin)."""
        L = self.L
        if L == 1:
            c_lane, c_fh, c_fa, c_fb = arg
            c_other = None
        else:
            c_lane, c_other, c_fh, c_fa, c_fb = arg
        bits = self.combo(lane, cin)

        def cls(p):
            if p >= self.ly.top:
                return c_fh
            if p == 0:
                return c_fa
            if p < BO:
                return c_fb if bits[p - 1] else c_fa
            return c_lane if (p - BO) // 8 == lane else c_other

        return self.ly.classes(cls)

    def index_of(self, lane, cin, sums):
        """Simulate: S with the given lane sums (other lanes: column sums of 0 bits)."""
        t = [SIGMA[0]] * TRITS
        for i, p in enumerate(self.ly.lane_pos(lane)):
            t[p] = SIGMA[sums[i]]
        s = V(t)
        seq = MASK_BYTE if self.L == 1 else MASK_WORD
        cells = simulate(seq, {"S": s, "I": C1}, lambda arg: self.mask_value(lane, cin, arg))
        assert cells["S"] == s
        return cells["I"]

    def add_routine(self, X, Y, Z):
        asm, ly, L = self.asm, self.ly, self.L
        S = [f"S{c}" for c in range(ly.ncells)]
        I = [f"I{j}" for j in range(4)]
        for s in S:
            asm.reg(s, C1)
        for i in I:
            asm.reg(i, C1, dispatch=True)
        fc = asm.flag("FC")
        fj = [asm.flag(f"FJ{j}") for j in range(4)]
        cont = [f"CONT{j}" for j in range(4)]
        seq = MASK_BYTE if L == 1 else MASK_WORD

        # Streams per (lane, result byte, carry-out): A := the merge constant, maybe set FC.
        streams = {}
        for lane in range(L):
            for r in range(256):
                for cout in (0, 1):
                    def cls(p, lane=lane, r=r):
                        if not BO <= p < ly.top:
                            return 0
                        ln, i = divmod(p - BO, 8)
                        return 1 if ln < lane else (1 + ((r >> i) & 1) if ln == lane else 0)
                    lines = []
                    asm.set(lines, ly.classes(cls))
                    carry = f" R_{fc}" if cout else ""
                    chain = " ".join(f"{fj[j]} {cont[j]} R_{fj[j]}" for j in range(4) if j % L == lane)
                    name = f"ST{lane}_{r}_{cout}"
                    streams[(lane, r, cout)] = name
                    asm.blocks.append(f"\n.DATA\n{name}: R_MD R_MD2 {lines[0]}{carry} {chain}")
        # Tables per (lane, carry-in): entry [I+1] = MD2, [I+2] = the stream.
        used = set()
        self.table_cells = 0
        for lane in range(L):
            for cin in (0, 1):
                for code in range(3 ** 8):
                    sums = [(code // 3 ** i) % 3 for i in range(8)]
                    idx = self.index_of(lane, cin, sums)
                    total = sum(s << i for i, s in enumerate(sums)) + cin
                    at = idx.inc()
                    assert at.head == 1 and at.width() < asm.width, at
                    assert at not in used and at.inc() not in used
                    used.update((at, at.inc()))
                    s = streams[(lane, total & 255, total >> 8)]
                    asm.blocks.append(f"@{at.lit()} {{ E{lane}_{cin}_{code}: MD2 {s} }}")
                    self.table_cells += 2
        self.table_used = used

        def body(out):
            for c in range(ly.ncells):
                asm.seq(out, RESET, {"S": Z[c]}, self.bh)
                asm.seq(out, SUM, {"X": X[c], "Y": Y[c], "S": S[c]}, self.bh)
            for j in range(4):
                c, lane = divmod(j, L)
                variants = (0, 1) if j else (0,)
                cin1, join = asm.label("CIN"), asm.label("JOIN")
                if j:
                    out.append(f"{fc} {cin1} R_{fc}")
                for cin in variants:
                    if cin:
                        out.append(f"{cin1}:")
                    asm.seq(out, seq, {"S": S[c], "I": I[j]},
                            lambda arg, lane=lane, cin=cin: self.mask_value(lane, cin, arg))
                    out.append(f"R_{fj[j]} MOVED {I[j]}_M")
                out.append(f"{cont[j]}: R_MOVED")
                asm.opr(out, Z[c])  # merge the lane into Z
                asm.seq(out, RESET, {"S": I[j]}, self.bh)
            out.append(f"{fc} {join} R_{fc}")  # drop the carry-out of the last lane
            out.append(f"{join}:")
            for c in range(ly.ncells):
                asm.set(out, C1)  # the unwritten trits (1) of Z become 0
                asm.opr(out, Z[c])
                asm.seq(out, RESET, {"S": S[c]}, self.bh)
        return self.subroutine("ADD", body)

    # RAM.
    def record_addr(self, a):
        """The record of RAM unit a (a byte address for L = 1, else a unit index): trit 0 = 2,
        trit 1 = 0, the bits of a from trit 2 as 0/1, head 1."""
        t = [1] * TRITS
        t[0], t[1] = 2, 0
        for i in range(20):
            t[2 + i] = (a >> i) & 1
        return V(t)

    def ram(self, a, value_cell):
        at = self.record_addr(a)
        lab = f"RAM{a}"
        self.asm.blocks.append(f"@{at.lit()} {{ {lab}: {value_cell.lit()} MB BACK }}")
        return at

    def ram_access(self, P):
        """Emit an access to the record P points at; the caller has set A."""
        sites = self.ram_sites

        def emit(out):
            self.asm.call(out, f"{P}_A", sites, "R_MOVED")
        return emit

    def ram_back(self):
        self.asm.blocks.append(lambda: "\n.DATA\n{\n" + "\n".join(
            ["BACK: R_MB R_RA ?- ?- R_RA ?- ?- R_RA ?- ?-"] + self.asm.chain(self.ram_sites)) + "\n}")

    def load_routine(self, P, Z, name="LOAD"):
        def body(out):
            for c in range(len(P)):
                self.asm.seq(out, MOV1, {"X": self.ram_access(P[c]), "Z": Z[c]}, self.bh)
        return self.subroutine(name, body)

    def store_routine(self, P, X):
        def body(out):
            for c in range(self.ly.ncells):
                self.asm.seq(out, MOV, {"X": X[c], "Z": self.ram_access(P[c])}, self.bh)
        return self.subroutine("STORE", body)

    def read_routine(self, name, P, sets):
        """A := the RAM cell at P, non-destructively: one access per constant in `sets`."""
        def body(out):
            for v in sets:
                self.asm.set(out, v)
                self.ram_access(P)(out)
        return self.subroutine(name, body)

    def rot_read(self, n):
        """A := the RAM cell by n Rot accesses (the cell is rotated back to its value). RR has no
        RNop, so its pointer is record - 1, and its records return through BACKR, which advances
        the 4-cycle MovD three times and the 6-cycle Rot five times."""
        at = self.record_addr(1000)
        self.asm.blocks.append(f"@{at.lit()} {{ RAMR: {self.ly.word(0x3C3C3C3C)[0].lit()} MB BACKR }}")
        self.asm.reg("PR", at.dec(), ram="rot")
        sites = []

        def body(out):
            for _ in range(n):
                self.asm.call(out, "PR_A", sites, "R_MOVED")
        call = self.subroutine("READROT", body)
        self.asm.blocks.append(lambda: "\n.DATA\n{\n" + "\n".join(
            ["BACKR: R_MB R_RR ?- R_RR ?- R_RR ?- R_RR2 R_RR2"] + self.asm.chain(sites)) + "\n}")
        return call, self.ly.word(0x3C3C3C3C)[0]

    # Pointers (word cells only): P := record(address) - 2, from the address register X.
    def ptr_consts(self):
        ly = self.ly

        def cls(used, low, high):
            # trit `top` follows the low class: records have it 0 and the tables 1, so they are apart
            return V([low if p < BO or p == ly.top else (used if p < ly.top else high)
                      for p in range(TRITS)])
        return cls(2, 0, 1), cls(2, 1, 1)

    def ptr_record(self, a):
        """The record a pointer built from address a points at (see ptr_routine)."""
        t = [1] * TRITS
        t[0], t[1], t[2], t[3] = 2, 0, 0, 0
        t[self.ly.top] = 0
        for i in range(32):
            t[BO + i] = 1 - ((a >> i) & 1)
        return V(t)

    def ptr_routine(self, X, P):
        K, R = self.ptr_consts()
        seq = RESET + [("SETV", K), ("OPR", "X"), ("OPR", "P"), ("SETV", R), ("OPR", "X")]

        def body(out):
            for op, arg in seq:
                if op == "SETV":
                    self.asm.set(out, arg)
                elif op == "SET":
                    self.asm.set(out, self.bh(arg))
                else:
                    self.asm.opr(out, {"X": X, "P": P, "S": P}[arg])
        return self.subroutine("PTR", body)

    def check_ptr(self, a, old):
        K, R = self.ptr_consts()
        X = self.ly.word(a)[0]
        cells = simulate(RESET, {"S": old}, self.bh)
        cells = simulate([("SET", K), ("OPR", "X"), ("OPR", "P"), ("SET", R), ("OPR", "X")],
                         {"X": X, "P": cells["S"]}, lambda v: v)
        assert cells["X"] == X
        assert cells["P"].inc().inc() == self.ptr_record(a), (cells["P"], self.ptr_record(a))


def check_sequences():
    """Simulate the sequences on random words (all lanes at once)."""
    rng = random.Random(7)
    ly = Layout(4)
    for _ in range(200):
        x, y, z = (rng.getrandbits(32) for _ in range(3))
        X, Y, Z = (ly.word(w)[0] for w in (x, y, z))
        bh = lambda arg: bits_head(ly, *arg)  # noqa: E731
        r = simulate(XOR, {"X": X, "Y": Y, "Z": Z}, bh)
        assert r["X"] == X and r["Y"] == Y and ly.decode(r["Z"]) == x ^ y
        for seq in (MOV, MOV1):
            r = simulate(seq, {"X": X, "Z": Z}, bh)
            assert r["X"] == X and r["Z"] == X
        r = simulate(SUM, {"X": X, "Y": Y, "S": C1}, bh)
        assert r["X"] == X and r["Y"] == Y
        for lane in range(4):
            for i, p in enumerate(ly.lane_pos(lane)):
                bit = lambda w: (w >> (8 * lane + i)) & 1  # noqa: E731
                assert r["S"].t[p] == SIGMA[bit(x) + bit(y)]
        assert all(r["S"].t[p] == SIGMA[0] for p in range(TRITS) if not BO <= p < ly.top)
        r2 = simulate(RESET, {"S": r["S"]}, bh)
        assert r2["S"] == C1


def main():
    L, out = int(sys.argv[1]), sys.argv[2]
    check_sequences()
    b = Bench(L)
    X, Y, Z = b.regs("X"), b.regs("Y"), b.regs("Z")
    xor = b.xor_routine(X, Y, Z)
    add = b.add_routine(X, Y, Z)
    b.ram_sites = []
    nrec = b.ly.ncells
    P = []
    for c in range(nrec):
        at = b.ram(c, b.ly.word(0x5A5A5A5A)[c])
        p = f"P{c}"
        b.asm.reg(p, at.dec().dec(), ram=True)
        P.append(p)
    load = b.load_routine(P, Z)
    store = b.store_routine(P, X)
    k2 = V.make(1, [2] * b.ly.top)  # the low trits 2: crz with it swaps the trits 1 and 2
    read_c1 = b.read_routine("READC1", P[0], [C1])
    read_c2 = b.read_routine("READC2", P[0], [k2, k2])
    read_rot, rot_value = b.rot_read(b.asm.width)
    rng = random.Random(3)
    vectors = [(0xFFFFFFFF, 1), (0x12345678, 0x9ABCDEF0), (0, 0)]
    vectors += [(rng.getrandbits(32), rng.getrandbits(32)) for _ in range(5)]
    if L == 4:
        b.asm.reg("PP", V.make(1), ram=True)
        ptr = b.ptr_routine(X[0], "PP")
        lw = b.load_routine(["PP"], Z, "LW")
        for x, y in vectors:
            a = x & 0xFFFFC
            b.check_ptr(a, V.make(1))
            rec = b.ptr_record(a)
            assert not {rec, rec.inc(), rec.inc().inc()} & b.table_used
            b.asm.blocks.append(f"@{rec.lit()} {{ W{a}: {b.ly.word(y ^ 0xA5A5A5A5)[0].lit()} MB BACK }}")
    b.ram_back()
    for x, y in vectors:
        b.load_word(X, x)
        b.load_word(Y, y)
        b.measure("xor", xor, Z, x ^ y)
        b.measure("add", add, Z, (x + y) & 0xFFFFFFFF)
        b.measure("store", store, [], None)
        b.load_word(Z, 0)
        b.measure("load", load, Z, x)
        b.measure_a("read_c1", read_c1, b.ly.word(x)[0])
        b.measure_a("read_c2", read_c2, b.ly.word(x)[0])
        b.measure_a("read_rot", read_rot, rot_value)
        if L == 4:
            b.load_word(X, x & 0xFFFFC)
            b.measure("ptr", ptr, [], None)
            b.load_word(Z, 0)
            b.measure("lw", lw, Z, y ^ 0xA5A5A5A5)
    with open(out, "w") as f:
        f.write(b.asm.render(b.main))
    with open(out.rsplit(".", 1)[0] + ".json", "w") as f:
        json.dump({"L": L, "ncells": b.ly.ncells, "top": b.ly.top, "expect": b.expect,
                   "table_cells": b.table_cells}, f)


if __name__ == "__main__":
    main()
