#!/usr/bin/env python3
"""Propose names for pgr3 functions by matching them against another
rexglue project's named functions (rr6's XDK 2135 D3D, by default).

Both codegen outputs are indexed with funcdb.py. Each named reference function
is compared with every pgr3 function in an address window by its instruction
shapes (mnemonic plus operands with immediates masked), and the best match is
printed with its score and the runner-up, so ambiguous ones stand out. Matches
are proposals: confirm each with `funcdb.py show` before naming it.

    match_names.py <ref_root> <ref_config.toml> [lo hi] [min_score]

e.g. match_names.py D:/rr6 D:/rr6/config/d3d.toml 0x823A0000 0x82400000
"""
import difflib
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import funcdb  # noqa: E402

IMM_RE = re.compile(r"(?<![rf])-?\b(0x[0-9a-fA-F]+|\d+)\b")


def index(root: Path, cache_name: str):
    funcdb.GEN = root / "generated" / "default"
    funcdb.CACHE = Path(__file__).with_name(cache_name)
    return funcdb.load()


def shape(asm):
    out = []
    for ins in asm:
        if ins.startswith("loc_"):
            continue
        op = ins.split(" ", 1)
        # Branch targets and constants differ between builds; registers mostly don't.
        out.append(op[0] + (" " + IMM_RE.sub("#", op[1]) if len(op) > 1 else ""))
    return out


def addr(name):
    m = re.fullmatch(r"sub_([0-9A-F]{8})", name)
    return int(m.group(1), 16) if m else None


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    ref_root, ref_cfg = Path(argv[1]), Path(argv[2])
    lo = int(argv[3], 16) if len(argv) > 3 else 0
    hi = int(argv[4], 16) if len(argv) > 4 else 0xFFFFFFFF
    min_score = float(argv[5]) if len(argv) > 5 else 0.0

    names = {int(a, 16): n for a, n in re.findall(
        r"(0x[0-9A-Fa-f]{8})\s*=\s*\{[^}]*name\s*=\s*\"(\w+)\"", ref_cfg.read_text())}
    ref = index(ref_root, ".funcdb_ref.pickle")["funcs"]
    own = index(Path(__file__).resolve().parents[2], ".funcdb.pickle")["funcs"]

    # A named function is emitted under its name, with the body at __imp__<name>.
    def ref_func(a, n):
        for key in (f"sub_{a:08X}", f"__imp__{n}", n):
            if key in ref:
                return ref[key]
        return None

    cands = [(n, shape(f["asm"])) for n, f in own.items()
             if (a := addr(n)) is not None and lo <= a < hi]
    print(f"# {len(names)} reference names, {len(cands)} candidates in "
          f"0x{lo:08X}..0x{hi:08X}")
    for a, n in sorted(names.items()):
        f = ref_func(a, n)
        if f is None:
            print(f"{n:44} (not in reference output)")
            continue
        s = shape(f["asm"])
        scored = []
        for cn, cs in cands:
            if not 0.6 * len(s) <= len(cs) <= 1.6 * len(s) + 4:
                continue
            sm = difflib.SequenceMatcher(None, s, cs, autojunk=False)
            if sm.real_quick_ratio() < 0.5 or sm.quick_ratio() < 0.5:
                continue
            scored.append((sm.ratio(), cn))
        scored.sort(reverse=True)
        if not scored or scored[0][0] < min_score:
            print(f"{n:44} no match (ref {len(s)} ins)")
            continue
        best, second = scored[0], scored[1] if len(scored) > 1 else (0.0, "-")
        print(f"{n:44} {best[1]}  {best[0]:.2f}   next {second[1]} {second[0]:.2f}"
              f"   ({len(s)} ins)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
