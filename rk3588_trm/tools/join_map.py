#!/usr/bin/env python3
"""join_map.py — TRM 位址表 × 板子 DT 清單：每個 TRM 區塊對到哪些 DT 節點、啟用與否、驅動。
用法：python3 join_map.py ../data/trm_addrmap.csv ../data/dt_inventory.csv > ../data/trm_vs_dt.csv"""
import csv, sys, re
def sz(s):
    m = re.match(r'([\d.]+)([KMG])B', s, re.I); u = {'K': 1 << 10, 'M': 1 << 20, 'G': 1 << 30}[m.group(2).upper()]
    return int(float(m.group(1)) * u)
trm = [(r['module'], int(r['start'], 16), sz(r['size']), r['size']) for r in csv.DictReader(open(sys.argv[1]))]
dt = list(csv.DictReader(open(sys.argv[2])))
w = csv.writer(sys.stdout)
w.writerow(['module', 'start', 'size', 'dt_nodes', 'status', 'drivers'])
for n, a, s, ss in trm:
    hit = [d for d in dt if a <= int(d['addr'], 16) < a + s]
    st = '/'.join(sorted({d['status'] for d in hit})) or 'no-node'
    w.writerow([n, '%08x' % a, ss, ' '.join(d['node'] for d in hit), st,
                ' '.join(sorted({d['driver'] for d in hit if d['driver']}))])
