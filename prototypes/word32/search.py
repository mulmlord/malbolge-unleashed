# THROWAWAY prototype code for the spike #18 (docs/adr/0001-hla-word-representation.md).
#
# Superoptimizer for trit-wise operations. crz acts on every trit position independently, so a word
# operation that is position-local (a bitwise op, a column sum, building a table index) is a short
# sequence of two primitive moves, found here by a cheapest-first search:
#   SET v   A := a constant, one trit per position class (an inline load, see tern.load_seq)
#   OPR k   A := k := crz(A, k) for a cell k of the routine's working set
# A *case* is one position class plus one assignment of the input trits; the state holds A and
# every cell as a tuple over all cases. The class holding the head may never get a constant trit 2
# (data words cannot have head 2, docs/asm.md 4.3).
#
# Usage: python3 search.py [problem ...]

import heapq
import itertools
import sys

from tern import T, load_cost

# Measured costs in steps (README.md): an Opr on a register is 9 steps plus 3 per skipped flag of
# its return chain (the search assumes none); a SET is 3 steps per inline op (tern.load_cost).
OPR_COST = 9


def crz(a, d):
    return tuple(T[dd][aa] for aa, dd in zip(a, d))


class Problem:
    """classes: list of (name, {input: domain}, has_head). cells: name -> f(class, inputs) giving
    the initial trit; goal: name -> f(class, inputs) giving the final trit, or None (anything);
    cells missing from goal must be kept."""

    def __init__(self, classes, inputs, cells, goal):
        self.classes = classes
        self.cases = []
        for ci, (_, domains, _) in enumerate(classes):
            for vals in itertools.product(*(domains[i] for i in inputs)):
                self.cases.append((ci, dict(zip(inputs, vals))))
        self.names = list(cells)
        self.start = tuple(tuple(cells[n](ci, iv) for ci, iv in self.cases) for n in self.names)
        self.goal = []
        for k, name in enumerate(self.names):
            g = goal.get(name, "keep")
            if g == "keep":
                self.goal.append(self.start[k])
            elif g is None:
                self.goal.append(None)
            else:
                self.goal.append(tuple(g(ci, iv) for ci, iv in self.cases))

    def consts(self):
        heads = [h for _, _, h in self.classes]
        out = []
        for per_class in itertools.product(range(3), repeat=len(self.classes)):
            cost = load_cost(per_class, heads)
            if cost is None:
                continue
            vec = tuple(per_class[ci] for ci, _ in self.cases)
            out.append((cost, per_class, vec))
        return out

    def done(self, cells):
        return all(g is None or g == c for g, c in zip(self.goal, cells))

    def solve(self, max_cost=300):
        consts = self.consts()
        start = (None, self.start)
        dist = {start: 0}
        prev = {start: None}
        heap = [(0, 0, start)]
        tie = 1
        while heap:
            cost, _, st = heapq.heappop(heap)
            if dist.get(st) != cost:
                continue
            a, cells = st
            if self.done(cells):
                path = []
                s = st
                while prev[s] is not None:
                    s, m = prev[s]
                    path.append(m)
                return cost, list(reversed(path))
            if cost > max_cost:
                return None
            moves = [(c, ("SET", per_class), (vec, cells)) for c, per_class, vec in consts]
            if a is not None:
                for k, cell in enumerate(cells):
                    r = crz(a, cell)
                    moves.append((OPR_COST, ("OPR", self.names[k]), (r, cells[:k] + (r,) + cells[k + 1:])))
            for c, m, s2 in moves:
                nc = cost + c
                if nc < dist.get(s2, 1 << 30):
                    dist[s2] = nc
                    prev[s2] = (st, m)
                    heapq.heappush(heap, (nc, tie, s2))
                    tie += 1
        return None


# Bits are stored as the trit 2*b ("f02"): with A = C1 an Opr on such a cell is the identity, so
# a register is read into A by one Opr.
BIT = (0, 1)
TRIT = (0, 1, 2)


def f02(b):
    return 2 * b


def bitwise(op):
    """Z := op(X, Y) for f02 words. Classes: the data bits, and the head (all inputs 0)."""
    zero = {"x": (0,), "y": (0,), "z": (0,)}
    return Problem(
        [("bits", {"x": BIT, "y": BIT, "z": BIT}, False), ("head", zero, True)],
        ["x", "y", "z"],
        {n: (lambda n: lambda c, i: f02(i[n.lower()]))(n) for n in "XYZ"},
        {"Z": lambda c, i: f02(op(i["x"], i["y"]))},
    )


def column_sum(sigma):
    """S := sigma(x + y) per position (the head too, with x = y = 0); S holds C1 before."""
    zero = {"x": (0,), "y": (0,)}
    return Problem(
        [("bits", {"x": BIT, "y": BIT}, False), ("head", zero, True)],
        ["x", "y"],
        {"X": lambda c, i: f02(i["x"]), "Y": lambda c, i: f02(i["y"]), "S": lambda c, i: 1},
        {"S": lambda c, i: sigma[i["x"] + i["y"]]},
    )


def reset(init):
    """A scratch cell holding any trit back to `init`."""
    return Problem(
        [("any", {"s": TRIT}, False), ("head", {"s": TRIT}, True)],
        ["s"],
        {"S": lambda c, i: i["s"]},
        {"S": lambda c, i: init},
    )


PROBLEMS = {
    "mov": lambda: bitwise(lambda x, y: x),
    "xor": lambda: bitwise(lambda x, y: x ^ y),
    "and": lambda: bitwise(lambda x, y: x & y),
    "or": lambda: bitwise(lambda x, y: x | y),
    "reset1": lambda: reset(1),
    "sum102": lambda: column_sum((1, 0, 2)),
    "sum120": lambda: column_sum((1, 2, 0)),
}


def show(name, res):
    if res is None:
        print(f"{name}: no solution", flush=True)
        return
    cost, path = res
    steps = " ; ".join(f"{op} {''.join(map(str, arg)) if op == 'SET' else arg}" for op, arg in path)
    print(f"{name}: cost {cost}: {steps}", flush=True)


if __name__ == "__main__":
    for name in sys.argv[1:] or list(PROBLEMS):
        show(name, PROBLEMS[name]().solve())
