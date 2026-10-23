"""Collapse one traced frame into passes: each SetRenderTarget/Depth, Clear,
tiling call and Resolve on its own line, with runs of draws counted.
usage: pass_list.py <log> <frame>"""
import re, sys
lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
frame = sys.argv[2]
on = False
draws = 0
def flush():
    global draws
    if draws:
        print(f"      {draws} draws")
        draws = 0
for l in lines:
    if "[xtrace]" not in l:
        continue
    body = l.split("[xtrace] ", 1)[1]
    m = re.search(r"frame=(\d+)", body)
    if m:
        on = m.group(1) == frame
    if not on:
        continue
    kind = body.split()[0]
    if kind in ("DrawIndexed", "Draw", "BeginVertices"):
        draws += 1
        continue
    flush()
    body = re.sub(r" t\d=0x[0-9A-F]+", "", body)
    body = re.sub(r" (vs|ps|decl)=0x[0-9A-F]+", "", body)
    body = re.sub(r" lr=0x[0-9A-F]+.*", "", body)
    print("  " + body[:230])
flush()
