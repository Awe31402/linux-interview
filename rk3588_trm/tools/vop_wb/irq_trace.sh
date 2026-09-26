#!/bin/sh
# irq_trace.sh [wbtest 參數…] — 用 kprobe/kretprobe 記錄 VOP 每次中斷讀到的 VP 中斷狀態（繞過 printk 限流），
# 跑一次 wbtest，然後統計每個 VP 的 FS_FIELD（bit15，每格一次）與 POST_BUF_EMPTY（bit12）次數。需 root。
T=/sys/kernel/tracing
echo 0 > $T/tracing_on; echo > $T/trace
echo 'p:vopirq/vpin vop2_read_and_clear_active_vp_irqs vp=%x1:s32' > $T/kprobe_events
echo 'r:vopirq/vpout vop2_read_and_clear_active_vp_irqs ret=$retval:x32' >> $T/kprobe_events
echo 1 > $T/events/vopirq/enable
echo 1 > $T/tracing_on
/tmp/vop_wb/wbtest "$@" | grep -E "MODE override|frame [0-9]|avg later|failed|===" 
echo 0 > $T/tracing_on
echo 0 > $T/events/vopirq/enable
cat $T/trace > /tmp/vop_irq_trace.txt
python3 - <<'PY'
import re, collections
vp = None; n = collections.Counter(); fs = collections.Counter(); pbe = collections.Counter()
t_fs = collections.defaultdict(list); t_pbe = collections.defaultdict(list)
for line in open('/tmp/vop_irq_trace.txt'):
    m = re.search(r'\s(\d+\.\d+): vpin:.*vp=(-?\d+)', line)
    if m: vp = int(m.group(2)); continue
    m = re.search(r'\s(\d+\.\d+): vpout:.*ret=0x([0-9a-f]+)', line)
    if m and vp is not None:
        t, r = float(m.group(1)), int(m.group(2), 16)
        if r:
            n[vp] += 1
            if r & 0x8000: fs[vp] += 1; t_fs[vp].append(t)
            if r & 0x1000: pbe[vp] += 1; t_pbe[vp].append(t)
for v in sorted(n):
    print('  vp%d: 非零中斷=%d  FS_FIELD=%d  POST_BUF_EMPTY=%d' % (v, n[v], fs[v], pbe[v]))
    if len(t_fs[v]) > 3:
        d = [b - a for a, b in zip(t_fs[v][1:], t_fs[v][2:])]   # 跳過第一個間隔
        d.sort(); med = d[len(d) // 2]
        print('       FS 間隔中位數 %.4f ms（%d 個樣本，最小 %.4f 最大 %.4f）' % (med * 1e3, len(d), d[0] * 1e3, d[-1] * 1e3))
    if t_fs[v]:
        t0 = t_fs[v][0]
        per = collections.Counter(sum(1 for f in t_fs[v] if f <= p) for p in t_pbe[v])
        print('       POST_BUF_EMPTY 落在第幾格（以 FS 計）：%s' % dict(sorted(per.items())))
        if t_pbe[v]:
            print('       第一次 POST_BUF_EMPTY 在第一個 FS 之後 %.2f ms，最後一次 %.2f ms；最後一個 FS %.2f ms' %
                  ((t_pbe[v][0] - t0) * 1e3, (t_pbe[v][-1] - t0) * 1e3, (t_fs[v][-1] - t0) * 1e3))
PY
echo > $T/kprobe_events
