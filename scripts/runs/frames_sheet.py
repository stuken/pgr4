"""From a folder of pgr4_dump_frames BMPs, keep each frame's front buffer
(the native build's pgr4_frame_<N>.bmp, else the xenos build's last 1280x720
resolve), save it as f<frame>.png and tile a labelled contact sheet.
usage: frames_sheet.py <bmp dir> <out dir> [cols]"""
import os, re, sys, collections
from PIL import Image, ImageDraw
src, out = sys.argv[1], sys.argv[2]; cols = int(sys.argv[3]) if len(sys.argv) > 3 else 5
os.makedirs(out, exist_ok=True)
last = {}
for f in os.listdir(src):
    m = re.match(r"pgr4_frame_(\d+)_r(\d+)_[0-9A-F]+(?:_s\d+)?_1280x720\.bmp", f)
    if m:
        fr, seq = int(m.group(1)), int(m.group(2))
        if fr not in last or seq > last[fr][0]:
            last[fr] = (seq, f)
    m = re.match(r"pgr4_frame_(\d+)\.bmp", f)
    if m:
        last[int(m.group(1))] = (1 << 30, f)
frames = sorted(last)
thumbs = []
for fr in frames:
    im = Image.open(os.path.join(src, last[fr][1])).convert("RGB")
    im.save(os.path.join(out, f"f{fr:05d}.png"))
    th = im.resize((384, 216)); ImageDraw.Draw(th).text((4, 4), str(fr), fill=(255, 0, 255))
    thumbs.append(th)
rows = (len(thumbs) + cols - 1) // cols
sheet = Image.new("RGB", (cols * 384, max(rows, 1) * 216))
for i, th in enumerate(thumbs): sheet.paste(th, ((i % cols) * 384, (i // cols) * 216))
sheet.save(os.path.join(out, "sheet.png"))
print(len(frames), "frames:", frames[0] if frames else None, "-", frames[-1] if frames else None)
