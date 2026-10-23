#!/bin/bash
# Custom Match run: Street Race in Tokyo (night) with a chosen weather and
# vehicle, tracing and dumping one race frame, with the shader census on.
#   custom_race.sh <name> "<weather keys>" "<vehicle keys>" <trace frame> [timeout s] [extra script]
# Weather keys move from Clear on the weather grid (row 1: clear, light rain,
# heavy rain, cloudy; row 2: snow, fog, heavy fog, storm; row 3: icy, showers),
# e.g. "right right" for heavy rain. Vehicle keys move from the default car in
# the class B list (e.g. "up" for the Monster S4RS bike); empty keeps the Enzo.
# [extra script] is appended to the pad script; "lb@160s,lb@166s" switches to
# the in-car view (LB twice, at least 4 s after the race starts). DUMPS overrides
# the dumped present frames (default: the menu pages, every 50 from 1100).
# CITY moves on the city grid from Tokyo (bottom right of three rows of three),
# e.g. CITY="up up left"; it adds about 16 s of menus, about 180 presents.
# Another city's previews load while its menus animate, and the menus drop
# presses meanwhile: STEP (default 4) spaces moves and SLACK pads each page
# change. London still lost presses at STEP=7 SLACK=12, so check the dumped
# menu pages before trusting a trace from another city.
#
# Runs the xenos build in out/build/win-amd64-relwithdebinfo and writes
# scratch/runs/<name>/: the logs (all.log is the two rotated files joined),
# the BMP dumps, f<frame>.png front buffers and sheet.png. It relies on the
# profile an earlier run created in <build>/user (the first boot adds ~40 s of
# profile screens) and on the timings measured in Phase 4: the race starts
# near present 1950 (about 2150 with a vehicle change) and the car is
# burning out against a wall by 2700, which is where to trace. The menus
# sometimes drop a press anyway (a run then ends in Arcade Mode or Advanced
# Options), so check sheet.png and rerun when the menu pages went astray.
set -e
slack=${SLACK:-0}
name=$1; wkeys=$2; vkeys=$3; trace=$4; tmo=${5:-330}; extra=$6
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=$ROOT/out/build/win-amd64-relwithdebinfo
R=$ROOT/scratch/runs/$name
rm -rf $R; mkdir -p $R/bmp
S="start@10s,start@15s,start@20s,start@25s,start@30s,start@40s,right@47s,right@51s,right@55s,start@60s"
S="$S,a@70s"
# A running clock: each move waits STEP s (default 4), and each page change
# SLACK s more on top of the gaps measured for Tokyo.
step=${STEP:-4}
t=78
press() { S="$S,$1@${t}s"; t=$((t+$2)); }
if [ -n "$CITY" ]; then
  press a $((6+slack))
  for k in $CITY; do press $k $step; done
  press a $((8+slack))
fi
base=$t
press down $step; press down $step; press a $((14+slack - 2*step))
for k in $wkeys; do press $k $step; done
# Confirm the weather 22 s after reaching the list, or a step after the last key.
# Then down to Lobby: exactly three moves, since the list wraps to City.
t=$(( t > base + 22 + slack ? t : base + 22 + slack ))
press a $((6+slack)); press down $step; press down $step; press down $((step+2+slack))
press a $((8+slack))
if [ -n "$vkeys" ]; then
  press a 8; press a 8
  for k in $vkeys; do press $k $step; done
  t=$((t+2)); press a 8
fi
S="$S,start@${t}s,rt@$((t+17))s+6000"
[ -n "$extra" ] && S="$S,$extra"
F="${DUMPS:-$(seq -s, 1100 50 2000)},$trace"
echo "$S" > $R/script.txt
cd $B
timeout $tmo ./pgr4.exe --log_file=$R/pgr4.log --pgr4_input_script=$S --readback_resolve=full \
  --d3d12_readback_resolve=true --pgr4_dump_frames=$F --pgr4_trace_frame=$trace \
  --pgr4_shader_census=true > /dev/null 2>&1 || true
mv user/pgr4_frame_* $R/bmp/ 2>/dev/null || true
cat $R/pgr4.1.log $R/pgr4.log > $R/all.log 2>/dev/null || cp $R/pgr4.log $R/all.log
python $ROOT/scripts/runs/frames_sheet.py $R/bmp $R 6
