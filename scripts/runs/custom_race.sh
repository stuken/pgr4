#!/bin/bash
# Custom Match run: Street Race in Tokyo (night) with a chosen weather and
# vehicle, tracing and dumping one race frame, with the shader census on.
#   custom_race.sh <name> "<weather keys>" "<vehicle keys>" <trace s> [timeout s] [extra script]
# <trace s> is when to dump and trace the race frame, in seconds from the
# front end's content check (pgr4_input_script wait:content#2), as every time
# in the script is: the race starts about 100 s after it (about 125 s with a
# vehicle change), and the car is burning out against a wall by about 140 s.
# Weather keys move from Clear on the weather grid (row 1: clear, light rain,
# heavy rain, cloudy; row 2: snow, fog, heavy fog, storm; row 3: icy, showers),
# e.g. "right right" for heavy rain. Vehicle keys move from the default car in
# the class B list (e.g. "up" for the Monster S4RS bike); empty keeps the Enzo.
# [extra script] is appended to the pad script, so its times count from the
# content check too; "lb@128s,lb@134s" switches to the in-car view (LB twice,
# at least 4 s after the race starts). DUMPS overrides the menu page dumps
# (default: every 4 s until the race starts), as a list of seconds.
# CITY moves on the city grid from Tokyo (bottom right of three rows of three),
# e.g. CITY="up up left"; it adds about 16 s of menus.
# Another city's previews load while its menus animate, and the menus drop
# presses meanwhile: STEP (default 4) spaces moves and SLACK pads each page
# change. London still lost presses at STEP=7 SLACK=12, so check the dumped
# menu pages before trusting a trace from another city.
#
# NATIVE=1 runs the native build (out/build/native-relwithdebinfo, at 1x)
# instead; it dumps the front buffer and every resolve target. With it,
# EXE=pgr4_vk.exe runs the Vulkan build. PGR4_ARGS adds options to its command line.
# Runs the xenos build in out/build/win-amd64-relwithdebinfo and writes
# scratch/runs/<name>/: the logs (all.log is the two rotated files joined),
# the BMP dumps, f<frame>.png front buffers and sheet.png. It relies on the
# profile an earlier run created in <build>/user (the first boot adds ~40 s of
# profile screens). The menus sometimes drop a press anyway (a run then ends
# in Arcade Mode or Advanced Options), so check sheet.png and rerun when the
# menu pages went astray.
set -e
slack=${SLACK:-0}
name=$1; wkeys=$2; vkeys=$3; trace=$4; tmo=${5:-330}; extra=$6
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=$ROOT/out/build/win-amd64-relwithdebinfo
[ -n "$NATIVE" ] && B=$ROOT/out/build/native-relwithdebinfo
R=$ROOT/scratch/runs/$name
rm -rf $R; mkdir -p $R/bmp
# Start dismisses the boot screens (the movies, the warning, the title, the
# profile dialog), however long each takes to load, until the content check
# (wait:content#2), about a second before the main menu appears; spare presses
# are dropped there. Every time after it counts from the content check.
S="$(seq -s, -f 'start@%gs' 10 4 90),wait:content#2"
S="$S,start@8s,right@15s,right@19s,right@23s,start@28s,a@38s"
# A running clock: each move waits STEP s (default 4), and each page change
# SLACK s more on top of the gaps measured for Tokyo.
step=${STEP:-4}
t=46
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
# Captures are script entries, timed like the presses, so they land on the
# same scene at any frame rate: every menu page, then the traced race frame.
D="${DUMPS:-$(seq -s, 10 4 $((t+10)))}"
S="$S,$(echo "$D" | sed -E 's/([0-9]+)/dump@\1s/g'),dump@${trace}s,trace@${trace}s"
echo "$S" > $R/script.txt
cd $B
if [ -n "$NATIVE" ]; then
  timeout $tmo ./${EXE:-pgr4.exe} --log_file=$R/pgr4.log --pgr4_input_script=$S \
    --pgr4_resolution_scale=1 $PGR4_ARGS > /dev/null 2>&1 || true
else
  timeout $tmo ./pgr4.exe --log_file=$R/pgr4.log --pgr4_input_script=$S --readback_resolve=full \
    --d3d12_readback_resolve=true --pgr4_shader_census=true > /dev/null 2>&1 || true
fi
mv user/pgr4_frame_* $R/bmp/ 2>/dev/null || true
cat $R/pgr4.1.log $R/pgr4.log > $R/all.log 2>/dev/null || cp $R/pgr4.log $R/all.log
python $ROOT/scripts/runs/frames_sheet.py $R/bmp $R 6
