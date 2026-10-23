"""Summarise a pgr4_trace_frame log: call counts per frame, every distinct
render target / depth surface with its decoded header, resolves, clears,
tiling, and the always-on 6132 events.  usage: trace_summary.py <pgr4.log>"""
import re, sys, collections
lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
ev = [l for l in lines if "[xtrace]" in l or "Direct3D_CreateDevice" in l or "CreateVertexShader" in l or "CreatePixelShader" in l]
print("always-on events:")
for l in ev:
    if re.search(r"Direct3D_CreateDevice|RunCommandBuffer|BeginZPass|Create(Vertex|Pixel)Shader", l) and "TraceCall" not in l:
        print("  ", l.split("] ", 4)[-1][:220])
kinds = collections.Counter()
surf = {}
for l in ev:
    m = re.search(r"\[xtrace\] (\w+)", l)
    if m: kinds[m.group(1)] += 1
    m = re.search(r"Set(RenderTarget \d+|DepthStencilSurface) (0x[0-9A-F]+)( \{[^}]*\})?", l)
    if m and m.group(3):
        surf[(m.group(1), m.group(2))] = m.group(3)
print("call counts over the traced frames:", dict(kinds))
print("surfaces:")
for (k, a), d in sorted(surf.items(), key=lambda x: x[0][1]):
    print(f"  {k:24} {a} {d}")
