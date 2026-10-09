# THROWAWAY prototype code for the spike #18 (docs/adr/0001-hla-word-representation.md).
#
# The table index of one lane for the add: from the column sums S (one ternary digit per bit
# position, search.py "sum"), build I with: the lane's positions a bijection of s, the other lanes
# of the cell one constant, the head and the trits above the lanes (fh) 1, so that the tables lie
# in the head-1 window, and two different constants on the fixed low trits fa and fb (they spell
# the lane and the carry-in). S is kept; I holds C1 before.
#
# Usage: python3 mask_search.py [byte|word]

import itertools
import sys

import search
from search import TRIT, Problem


class Mask(Problem):
    def done(self, cells):
        s, i = cells
        if s != self.start[0]:
            return False
        vals = {}
        for (ci, iv), v in zip(self.cases, i):
            vals.setdefault(ci, set()).add(v)
        if len(vals[0]) != 3:
            return False
        if any(len(vals[ci]) != 1 for ci in range(1, len(self.classes))):
            return False
        k = len(self.classes)
        return vals[k - 3] == {1} and vals[k - 2] != vals[k - 1]


def problem(kind):
    one = {"s": (1,)}  # the sum digit of two 0 bits
    classes = [("lane", {"s": TRIT}, False)]
    if kind == "word":
        classes.append(("other", {"s": TRIT}, False))
    classes += [("fh", one, True), ("fa", one, False), ("fb", one, False)]
    return Mask(classes, ["s"], {"S": lambda c, i: i["s"], "I": lambda c, i: 1}, {})


class Core(Problem):
    """The word mask without the fixed classes: lane a bijection, other lanes a constant."""

    def done(self, cells):
        s, i = cells
        if s != self.start[0]:
            return False
        vals = {}
        for (ci, iv), v in zip(self.cases, i):
            vals.setdefault(ci, set()).add(v)
        return len(vals[0]) == 3 and len(vals[1]) == 1


def fixed_class(skeleton, goal_ok, has_head):
    """Constants for one fixed class (s = 1, I = 1) along the skeleton: each SET picks a trit."""
    sets = [k for k, (op, _) in enumerate(skeleton) if op == "SET"]
    for choice in itertools.product(range(3), repeat=len(sets)):
        if has_head and 2 in choice:
            continue
        a, s, i = None, 1, 1
        it = iter(choice)
        for op, arg in skeleton:
            if op == "SET":
                a = next(it)
            elif arg == "S":
                s = a = search.T[s][a]
            else:
                i = a = search.T[i][a]
        if s == 1 and goal_ok(i):
            yield choice, i


def word():
    core = Core([("lane", {"s": TRIT}, False), ("other", {"s": TRIT}, False)], ["s"],
                {"S": lambda c, i: i["s"], "I": lambda c, i: 1}, {}).solve(400)
    search.show("mask word core", core)
    skel = core[1]
    fh = next(fixed_class(skel, lambda i: i == 1, True), None)
    fixed = list(fixed_class(skel, lambda i: True, False))
    print("fh", fh)
    print("fixed classes (constants, final I):", sorted({(c, i) for c, i in fixed}, key=lambda x: x[1]))


if __name__ == "__main__":
    kind = sys.argv[1] if len(sys.argv) > 1 else "byte"
    if kind == "word":
        word()
    else:
        search.show(f"mask {kind}", problem(kind).solve(400))
