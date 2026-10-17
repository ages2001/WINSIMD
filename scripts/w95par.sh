#!/usr/bin/env bash
# w95par.sh <seconds> a.vxd[@cpu] b.vxd[@cpu] ...  (cpu default pentium3) - boot several VxD variants in parallel,
# each on its own sparse copy of the Win95 test image (Logged boot is forced
# via MSDOS.SYS), then print each variant's BOOTLOG verdict + E9 output.
set -u
T=$1; shift
IMG=/home/claude/vhd/win95/win95.raw
W=/tmp/claude-0/-home-claude/391263ed-6c76-506e-aaac-cb8c3db031ea/scratchpad/par
rm -rf $W; mkdir -p $W
i=0
for v in "$@"; do
  i=$((i+1)); d=$W/$i; mkdir -p $d
  cp --sparse=always $IMG $d/disk.raw
  printf 'drive x: file="%s" offset=32256\nmtools_skip_check=1\n' $d/disk.raw > $d/mt.conf
  cpu=pentium3; case $v in *@*) cpu=${v#*@}; v=${v%@*};; esac; echo $cpu > $d/cpu
  MTOOLSRC=$d/mt.conf mcopy -o "$v" x:/WINDOWS/SYSTEM/WINSIMD.VXD
  MTOOLSRC=$d/mt.conf mdel x:/BOOTLOG.TXT 2>/dev/null
  echo "$v" > $d/name
done
for d in $W/*/; do
  timeout "$T" qemu-system-i386 -machine pc,acpi=off -m 64 -cpu $(cat ${d}cpu) \
    -drive file=${d}disk.raw,format=raw,if=ide -debugcon file:${d}e9.log \
    -vga std -display none -monitor unix:${d}mon.sock,server,nowait -no-reboot -boot c >/dev/null 2>&1 &
  # pick "2. Logged" in the startup menu (after a failed boot Win95 defaults to Safe mode)
  ( for t in 4 2 2 2; do sleep $t; printf 'sendkey 2\nsendkey ret\n' | nc -U -q1 ${d}mon.sock; done
    sleep $((T-14)); echo "screendump ${d}end.ppm" | nc -U -q1 ${d}mon.sock ) >/dev/null 2>&1 &
done
sleep $((T+2))
for d in $W/*/; do
  echo "=== $(basename $(cat ${d}name)) [$(cat ${d}cpu)] : $(MTOOLSRC=${d}mt.conf mtype x:/BOOTLOG.TXT 2>/dev/null | tr -d '\r' | grep -i 'winsimd' | grep -v Loading | head -3 | tr '\n' ' ')"
  echo "    E9: $(cat -v ${d}e9.log | tr '\n' ' ')"
done
