#!/usr/bin/env bash
# w95e2e.sh <stage> <run-program> <cpu> <seconds> <shotprefix>
# Boots the Win95 test image (Logged boot), runs <run-program> via WIN.INI
# run=, takes screendumps, then shuts Windows down cleanly via the Start menu
# (so registry/disk caches are flushed). Prints BOOTLOG winsimd lines + E9.
set -u
RUN=$2; CPU=$3; T=$4; SHOT=$5
IMG=/home/claude/vhd/win95/win95.raw
export MTOOLSRC=/tmp/mt_w95.conf
S=/tmp/claude-0/-home-claude/391263ed-6c76-506e-aaac-cb8c3db031ea/scratchpad
mtype x:/WINDOWS/WIN.INI | RUN="$RUN" python3 -c 'import os,sys,re; d=sys.stdin.buffer.read(); sys.stdout.buffer.write(re.sub(rb"(?m)^run=[^\r\n]*", lambda m: b"run="+os.environ["RUN"].encode(), d, count=1))' > $S/win.ini && mcopy -o $S/win.ini x:/WINDOWS/WIN.INI
mdel x:/BOOTLOG.TXT 2>/dev/null
: > /tmp/e9_w95.log; rm -f /tmp/mone2e.sock
M=/tmp/mone2e.sock
mon() { printf '%s\n' "$@" | nc -U -q1 $M >/dev/null 2>&1; }
(
  for t in 4 2 2 2; do sleep $t; mon "sendkey 2" "sendkey ret"; done   # t=10
  sleep $((T-75)); mon "screendump ${SHOT}_0.ppm"                      # T-65
  sleep 25; mon "screendump ${SHOT}_1.ppm"                             # T-40
  sleep 8;  mon "screendump ${SHOT}_2.ppm"                             # T-32
  sleep 2;  mon "sendkey ctrl-esc"; sleep 2; mon "sendkey u"; sleep 3; mon "sendkey ret"
  sleep 20; mon "screendump ${SHOT}_3.ppm"                             # T-5
) &
timeout $T qemu-system-i386 -machine pc,acpi=off -m 64 -cpu $CPU \
  -drive file=$IMG,format=raw,if=ide -debugcon file:/tmp/e9_w95.log \
  -vga std -display none -monitor unix:$M,server,nowait -no-reboot -boot c >/dev/null 2>&1
wait
echo "--- BOOTLOG ---"; mtype x:/BOOTLOG.TXT 2>/dev/null | tr -d '\r' | grep -i winsimd
echo "--- E9 ---"; cat -v /tmp/e9_w95.log; echo
for i in 0 1 2 3; do [ -f ${SHOT}_$i.ppm ] && python3 -c "from PIL import Image; Image.open('${SHOT}_$i.ppm').save('${SHOT}_$i.png')"; done
ls ${SHOT}_*.png 2>/dev/null
