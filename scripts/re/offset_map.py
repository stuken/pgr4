#!/usr/bin/env python3
"""Translate struct offsets from another rexglue project's XDK to pgr4's.

Pairs every function in a reference address window (pgr3's D3D library, say)
with its best instruction-shape match in a pgr4 window, as match_names.py
does, keeps the confident pairs, aligns their instructions, and for each
aligned pair that differs only in a load/store displacement records
reference offset -> pgr4 offset. Displacements off r1 (the stack) are left
out. Prints the mapping for the requested offsets with the number of
instruction pairs that vote for each answer, so a field that moved shows
up as a consistent shift and an unseen one as missing.

    offset_map.py <ref_root> <ref_lo> <ref_hi> <lo> <hi> [min_score] [offsets...]

e.g. offset_map.py D:/pgr3 0x823C0000 0x823E0000 0x826A0000 0x82720000 0.9 0x2D74 0x3290
With no offsets, prints every reference offset in 0..0x6000 seen at least twice.
"""
import collections
import difflib
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import funcdb  # noqa: E402
from match_names import addr, index, shape  # noqa: E402

MEM_RE = re.compile(r"^(l|st)[a-z]*\s+[rf]\d+,(-?\d+)\(r(\d+)\)$")


def mem_operand(ins):
    m = MEM_RE.match(ins)
    return (int(m.group(2)), int(m.group(3))) if m else None


def strip(asm):
    return [a for a in asm if not a.startswith("loc_")]


def main(argv):
    if len(argv) < 6:
        print(__doc__)
        return 1
    ref_root = Path(argv[1])
    ref_lo, ref_hi, lo, hi = (int(a, 16) for a in argv[2:6])
    min_score = float(argv[6]) if len(argv) > 6 else 0.9
    wanted = [int(a, 16) for a in argv[7:]]

    ref = index(ref_root, ".funcdb_ref.pickle")["funcs"]
    own = index(Path(__file__).resolve().parents[2], ".funcdb.pickle")["funcs"]
    refs = [(n, f) for n, f in ref.items() if (a := addr(n)) and ref_lo <= a < ref_hi]
    cands = [(n, shape(f["asm"]), f) for n, f in own.items() if (a := addr(n)) and lo <= a < hi]
    # Named functions are emitted as __imp__<name>; recover their bodies too.
    refs += [(n, f) for n, f in ref.items() if n.startswith("__imp__")]

    votes = collections.defaultdict(collections.Counter)
    pairs = 0
    for rn, rf in refs:
        rs = shape(rf["asm"])
        if len(rs) < 6:
            continue
        best = (0.0, None)
        for cn, cs, cf in cands:
            if not 0.8 * len(rs) <= len(cs) <= 1.25 * len(rs):
                continue
            sm = difflib.SequenceMatcher(None, rs, cs, autojunk=False)
            if sm.real_quick_ratio() < min_score or sm.quick_ratio() < min_score:
                continue
            r = sm.ratio()
            if r > best[0]:
                best = (r, cf)
        if best[0] < min_score:
            continue
        pairs += 1
        ra, ca = strip(rf["asm"]), strip(best[1]["asm"])
        sm = difflib.SequenceMatcher(None, shape(rf["asm"]), shape(best[1]["asm"]), autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag != "equal":
                continue
            for k in range(i2 - i1):
                a, b = mem_operand(ra[i1 + k]), mem_operand(ca[j1 + k])
                if a and b and a[1] == b[1] and a[1] != 1:
                    votes[a[0]][b[0]] += 1

    print(f"# {pairs} function pairs at score >= {min_score}")
    keys = wanted or [o for o in sorted(votes) if 0 <= o < 0x6000 and sum(votes[o].values()) >= 2]
    for o in keys:
        v = votes.get(o)
        if not v:
            print(f"0x{o:04X} -> (not seen)")
            continue
        top = v.most_common(3)
        shift = top[0][0] - o
        print(f"0x{o:04X} -> 0x{top[0][0]:04X} ({top[0][1]} votes{', shift %+#x' % shift if shift else ''})"
              + "".join(f"  alt 0x{x:04X} ({n})" for x, n in top[1:]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
