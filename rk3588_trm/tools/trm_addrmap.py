#!/usr/bin/env python3
"""trm_addrmap.py — 把 TRM Part1 Table 1-1（Address Mapping）從 pdftotext 文字檔解析成 CSV。
用法：python3 trm_addrmap.py books/rk3588_trm/part1/chapter_01.txt > addrmap.csv"""
import re, sys
lines = open(sys.argv[1], encoding='utf-8', errors='replace').read().split('\n')
start = next(i for i, l in enumerate(lines) if l.startswith('Table 1-1'))
end = next(i for i, l in enumerate(lines) if l.startswith('1.2 ') or l.startswith('1.3 '))
tok = [l.strip() for l in lines[start + 1:end] if l.strip()]
tok = [t for t in tok if t not in ('Module', 'Start Address', 'Size') and not t.startswith('RK3588 TRM') and not re.fullmatch(r'\d+', t) and 'Copyright' not in t and not t.startswith('Chapter')]
hexre = re.compile(r'^[0-9A-Fa-f]{8}$')
sizere = re.compile(r'^\d+(\.\d+)?\s*[KMG]B$', re.I)
out = []
i = 0
while i + 2 < len(tok):
    if hexre.match(tok[i + 1]) and sizere.match(tok[i + 2]):
        out.append((tok[i], int(tok[i + 1], 16), tok[i + 2].replace(' ', '')))
        i += 3
    else:
        i += 1
out.sort(key=lambda x: x[1])
print('module,start,size')
for n, a, s in out:
    print(f'{n},{a:08x},{s}')
