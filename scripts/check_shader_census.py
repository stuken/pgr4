#!/usr/bin/env python3
"""Compare a pgr4_shader_census run with the build's extracted shader set.

    check_shader_census.py <census_dir> <shader_dir>

<census_dir> is <user>/shader_census from a run with --pgr4_shader_census=true;
<shader_dir> is generated/shaders in the build directory. Every container the
game created must match, byte for byte, the extracted file for its address
(directly, or through an alias address in shaders.csv). Containers the game
builds on its stack have no fixed address, so those match by content against
the stack-built entries. Prints coverage: how
many of the extracted shaders the run created. Exits 1 on any mismatch or
on a container the extraction does not have.
"""
import csv
import sys
from pathlib import Path


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    census, shaders = Path(argv[1]), Path(argv[2])
    by_va = {}  # VA -> extracted file name
    stack_built = {}  # bytes -> extracted file name
    with open(shaders / "shaders.csv", newline="") as f:
        for row in csv.DictReader(f):
            if row.get("origin", "image").startswith("stack"):
                stack_built[(shaders / row["file"]).read_bytes()] = row["file"]
                continue
            by_va[int(row["va"], 16)] = row["file"]
            for alias in row["alias_vas"].split():
                by_va[int(alias, 16)] = row["file"]

    bad, missing, covered = [], [], set()
    for path in sorted(census.glob("*s_*.?so")):
        va = int(path.stem.split("_")[1], 16)
        name = by_va.get(va) or stack_built.get(path.read_bytes())
        if name is None:
            missing.append(path.name)
        elif (shaders / name).read_bytes() != path.read_bytes():
            bad.append(f"{path.name} != {name}")
        else:
            covered.add(name)

    total = len(list(shaders.glob("*s_*.?so")))
    created = len(list(census.glob("*s_*.?so")))
    print(f"{created} containers created: {len(covered)} of {total} extracted shaders "
          f"covered, {len(bad)} differ, {len(missing)} not extracted")
    for line in bad[:20]:
        print("  differs:", line)
    for name in missing[:20]:
        print("  not extracted:", name)
    return 1 if bad or missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
