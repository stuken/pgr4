#!/usr/bin/env python3
"""Pair pgr3's and pgr4's render- and sampler-state setters and report how
each one's device stores moved.

    state_setters.py <pgr3 root> <pgr3 image> <pgr4 image> [csv out]

The images are the patched ones pgr*_dump_image writes
(out/build/<preset>/generated/default.xex.image). Both titles keep the
{Get, Set, Default} tables D3D__InitializeStateTables walks, 12 bytes an
entry, but the render-state enum changed between XDK 2099 and 6132: 2099's
entry 57 is gone, 58-96 moved down one, and 6132 adds 96-100. So each pgr4
setter is paired with its best instruction-shape match among all of pgr3's
setters, and the index that pairing implies is printed beside it. The stores
and loads off the device (r3 at entry, often moved to r31) are aligned by
shape and listed as pgr3 offset -> pgr4 offset.
"""
import csv
import difflib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from match_names import index, shape  # noqa: E402
from offset_map import mem_operand, strip  # noqa: E402

BASE = 0x82000000
TABLES = {  # (pgr3 VA, entries, pgr4 VA, entries)
    "render": (0x825271F0, 97, 0x82A640D0, 101),
    "sampler": (0x82527680, 20, 0x82A64590, 20),
}


def read_table(image, va, n):
    return [struct.unpack_from(">3I", image, va - BASE + 12 * i) for i in range(n)]


def func(db, va, names):
    return db.get(f"sub_{va:08X}") or db.get(names.get(va, ""))


def moves(ra_asm, pa_asm):
    ra, pa = strip(ra_asm), strip(pa_asm)
    sm = difflib.SequenceMatcher(None, shape(ra_asm), shape(pa_asm), autojunk=False)
    out = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            for k in range(i2 - i1):
                a, b = mem_operand(ra[i1 + k]), mem_operand(pa[j1 + k])
                if a and b and a[1] == b[1] and a[1] != 1 and a[0] >= 0x400:
                    out.append((a[0], b[0], ra[i1 + k].split()[0]))
    return sm.ratio(), list(dict.fromkeys(out))


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 1
    pgr3_root, pgr3_img, pgr4_img = Path(argv[1]), Path(argv[2]).read_bytes(), Path(argv[3]).read_bytes()
    ref = index(pgr3_root, ".funcdb_ref.pickle")["funcs"]
    own = index(Path(__file__).resolve().parents[2], ".funcdb.pickle")["funcs"]
    import funcdb  # noqa: E402
    by_addr = {int(v[4:], 16): "__imp__" + k for k, v in funcdb.config_names().items()}

    rows = []
    for kind, (rva, rn, pva, pn) in TABLES.items():
        rt, pt = read_table(pgr3_img, rva, rn), read_table(pgr4_img, pva, pn)
        rsets = [(j, func(ref, e[1], {})) for j, e in enumerate(rt)]
        for i, (pget, pset, pdef) in enumerate(pt):
            pf = func(own, pset, by_addr)
            if not pf:
                rows.append([kind, i, f"0x{pset:08X}", "", "", f"0x{pdef:X}", "", "missing function"])
                continue
            ps = shape(pf["asm"])
            best = (0.0, None, None)
            for j, rf in rsets:
                if rf is None:
                    continue
                r = difflib.SequenceMatcher(None, shape(rf["asm"]), ps, autojunk=False).ratio()
                if r > best[0]:
                    best = (r, j, rf)
            r, j, rf = best
            _, mv = moves(rf["asm"], pf["asm"]) if rf else (0, [])
            rows.append([kind, i, f"0x{pset:08X}", j, f"{r:.2f}", f"0x{pdef:X}",
                         f"0x{rt[j][2]:X}" if j is not None else "",
                         " ".join(f"{m}:{a:#06x}->{b:#06x}" for a, b, m in mv)])
    out = csv.writer(open(argv[4], "w", newline="")) if len(argv) > 4 else None
    for r in rows:
        if out:
            out.writerow(r)
        print(f"{r[0]:7} {r[1]:3} {r[2]} <- pgr3 {str(r[3]):>3} {r[4]:>4} def {r[5]}/{r[6]:10} {r[7]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
