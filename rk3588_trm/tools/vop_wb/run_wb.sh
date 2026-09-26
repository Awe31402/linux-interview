#!/bin/sh
# run_wb.sh — 在板子上以 root、脫離 ssh 執行：
#   sudo setsid sh /tmp/vop_wb/run_wb.sh </dev/null >/tmp/vop_wb/run.log 2>&1 &
# 1) 看門狗安全網：開 /dev/watchdog0、設 60 s、magic close → 核心活著就自動餵；核心若被弄當，約 89 s 後自動重開
# 2) chvt 到 VT6，讓 logind 放掉 DRM master；3) 每個情境前寫麵包屑並 sync；4) 一律切回原本的 VT
D=/tmp/vop_wb; BC=/home/radxa/wb_breadcrumb
OLDVT=$(cat /sys/class/tty/tty0/active | tr -dc 0-9)
crumb() { echo "$(date +%T) $*" >> $BC; sync; }
python3 - <<'PY'
import fcntl, os, struct
fd = os.open('/dev/watchdog0', os.O_WRONLY)
fcntl.ioctl(fd, 0xC0045706, struct.pack('i', 60), False)
os.write(fd, b'V'); os.close(fd)
PY
crumb "wdt armed (magic-closed), oldvt=$OLDVT"
chvt 6; sleep 1
crumb "switched to vt6"
for s in ${SCENARIOS:-compose_cov compose_pre upscale2x wb_xhalf wb_yhalf fmt_bgr888 fmt_rgb565 fmt_nv12 timing}; do
	crumb "start $s"
	$D/wbtest $s
	crumb "end $s rc=$?"
done
chvt ${OLDVT:-2}
crumb "back to vt$OLDVT"
dmesg | grep -iE "vop|writeback|wb" | tail -20
