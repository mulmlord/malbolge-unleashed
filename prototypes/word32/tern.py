# THROWAWAY prototype code for the spike #18 (docs/adr/0001-hla-word-representation.md).
# A small model of malbolge-unleashed values (docs/spec.md section 1 and 3): 40 trits, trit 39 is
# the head. Used to compute constants and table addresses for the generated .mua prototypes.

TRITS = 40

# crz trit table, T[d][a] (spec 3.2): rows are the trit of [D], columns the trit of A.
T = ((1, 0, 0), (1, 0, 2), (2, 2, 1))


class V:
    """A 3-adic value: a tuple of 40 trits, index 39 is the repeating head."""

    __slots__ = ("t",)

    def __init__(self, trits):
        assert len(trits) == TRITS
        self.t = tuple(trits)

    @staticmethod
    def make(head, low=()):
        """head plus low trits (low[0] is trit 0)."""
        t = [head] * TRITS
        for i, x in enumerate(low):
            t[i] = x
        return V(t)

    @staticmethod
    def nat(k):
        assert k >= 0
        t = []
        for _ in range(TRITS):
            t.append(k % 3)
            k //= 3
        assert k == 0
        return V(t)

    @property
    def head(self):
        return self.t[TRITS - 1]

    def width(self):
        h = self.head
        w = 0
        for i in range(TRITS):
            if self.t[i] != h:
                w = i + 1
        return w

    def __eq__(self, o):
        return isinstance(o, V) and self.t == o.t

    def __hash__(self):
        return hash(self.t)

    def inc(self):
        t = list(self.t)
        for i in range(TRITS):
            if t[i] < 2:
                t[i] += 1
                return V(t)
            t[i] = 0
        raise OverflowError

    def dec(self):
        t = list(self.t)
        for i in range(TRITS):
            if t[i] > 0:
                t[i] -= 1
                return V(t)
            t[i] = 2
        raise OverflowError

    def add(self, k):
        v = self
        for _ in range(k):
            v = v.inc()
        return v

    def lit(self):
        """asm literal: 0t... for head 0, h t... otherwise (docs/asm.md 4.2)."""
        h = self.head
        w = max(self.width(), 1)
        digits = "".join(str(self.t[i]) for i in reversed(range(w)))
        return f"{h}t{digits}" if h else f"0t{digits}"

    def __repr__(self):
        return self.lit()


def crz(a, d):
    return V([T[d.t[i]][a.t[i]] for i in range(TRITS)])


def rotr(v, n):
    t = list(v.t)
    low = t[:n]
    low = low[1:] + low[:1]
    return V(low + t[n:])


C0 = V.make(0)
C1 = V.make(1)
C2 = V.make(2)


# Loading a constant into A without destroying anything: start with a Rot of a rotation fixed point
# (C0 or C1), then Opr on inline constants that are fixed points for the A they meet: with A = a,
# crz(a, k) = k exactly for the trits k in FIX[a]. A data word may not have head 2.
FIX = {0: (1, 2), 1: (0, 2), 2: (0,)}
MAX_LOAD = 4


def _paths(start, k, target, no2):
    """A list of k constant trits leading from start to target, or None."""
    def step(a):
        return [d for d in FIX[a] if not (no2 and d == 2)]

    def go(a, left):
        if left == 0:
            return [] if a == target else None
        for d in step(a):
            rest = go(d, left - 1)
            if rest is not None:
                return [d] + rest
        return None

    return go(start, k)


def load_cost(per_class, heads):
    """Steps to load a per-class constant (3 per inline op), or None."""
    best = None
    for start in (0, 1):
        for k in range(MAX_LOAD + 1):
            if all(_paths(start, k, t, h) is not None for t, h in zip(per_class, heads)):
                c = 3 * (1 + k)
                best = c if best is None else min(best, c)
                break
    return best


def load_seq(target):
    """(start, [k1, k2, ...]): Rot of the uniform start constant, then Opr on each k."""
    for k in range(MAX_LOAD + 1):
        for start in (0, 1):
            w = target.width()
            paths = [_paths(start, k, target.t[i], i >= w) for i in range(TRITS)]
            if all(p is not None for p in paths):
                consts = [V([paths[i][j] for i in range(TRITS)]) for j in range(k)]
                a = V.make(start)
                for c in consts:
                    assert crz(a, c) == c and c.head != 2
                    a = c
                assert a == target
                return V.make(start), consts
    raise ValueError(f"cannot load {target}")
