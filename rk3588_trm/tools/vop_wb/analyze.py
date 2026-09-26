#!/usr/bin/env python3
"""analyze.py — 分析 wbtest 寫回的影像（/tmp/wb_<name>.raw），對照預期值。用法：python3 analyze.py <name>..."""
import struct, sys
def load(name):
    return open('/tmp/wb_%s.raw' % name, 'rb').read()
def argb(d, pitch, x, y):
    b, g, r, a = d[y * pitch + x * 4: y * pitch + x * 4 + 4]
    return r, g, b, a
def grad(x, y, w=1920, h=1080):
    return (x * 255 // (w - 1), y * 255 // (h - 1), 64)

def compose(name):
    d = load(name); pitch = 7680
    garbage = sum(1 for y in range(0, 1080, 7) for x in range(0, 1920, 7) if argb(d, pitch, x, y) == (0x5a, 0x5a, 0x5a, 0x5a))
    print('[%s] 仍是填充值 0x5a 的取樣點：%d' % (name, garbage))
    err = mx = n = 0
    for y in range(0, 1080, 5):
        for x in range(0, 1920, 5):
            if 200 <= x < 712 and 200 <= y < 712: continue
            r, g, b, a = argb(d, pitch, x, y); e = grad(x, y)
            dd = max(abs(r - e[0]), abs(g - e[1]), abs(b - e[2])); mx = max(mx, dd); err += dd > 1; n += 1
    print('  overlay 外：%d 點，誤差>1 的 %d 點，最大誤差 %d；alpha 值樣本 %d' % (n, err, mx, argb(d, pitch, 100, 100)[3]))
    A = 0x80 / 255
    fits = {'coverage': [0, 0], 'premult': [0, 0]}
    samples = []
    for y in range(210, 700, 37):
        for x in range(210, 700, 41):
            r, g, b, a = argb(d, pitch, x, y); e = grad(x, y)
            cov = (round(e[0] * (1 - A)), round(e[1] * (1 - A)), round(255 * A + e[2] * (1 - A)))
            pre = (round(e[0] * (1 - A)), round(e[1] * (1 - A)), min(255, round(255 + e[2] * (1 - A))))
            for k, v in (('coverage', cov), ('premult', pre)):
                fits[k][0] += max(abs(r - v[0]), abs(g - v[1]), abs(b - v[2])); fits[k][1] += 1
            if len(samples) < 4: samples.append(((x, y), (r, g, b, a), cov, pre))
    for s in samples: print('  (x,y)=%s 實際 RGBA=%s  coverage 預期=%s  premult 預期=%s' % s)
    for k, (s, c) in fits.items(): print('  %-8s 平均誤差 %.2f' % (k, s / c))

def upscale(name):
    d = load(name); pitch = 7680
    print('[%s] 來源 960x540：R 奇偶欄 0/255、G 奇偶列 0/255 → 放大 2 倍' % name)
    print('  第 100 列 x=100..115 的 R：', [argb(d, pitch, x, 100)[0] for x in range(100, 116)])
    print('  第 100 欄 y=100..115 的 G：', [argb(d, pitch, 100, y)[1] for y in range(100, 116)])

def xhalf(name):
    d = load(name); pitch = 3840
    print('[%s] 來源 1920 寬 R 奇偶欄 0/255 → 寫回 960 寬' % name)
    print('  第 100 列 x=100..115 的 R：', [argb(d, pitch, x, 100)[0] for x in range(100, 116)])

def yhalf(name):
    d = load(name); pitch = 7680
    print('[%s] 來源 1080 高 G 奇偶列 0/255 → 寫回 540 高' % name)
    print('  第 100 欄 y=100..115 的 G：', [argb(d, pitch, 100, y)[1] for y in range(100, 116)])

BARS = ['black', 'white', 'red', 'green', 'blue', 'cyan', 'magenta', 'yellow']
BARRGB = [(0, 0, 0), (255, 255, 255), (255, 0, 0), (0, 255, 0), (0, 0, 255), (0, 255, 255), (255, 0, 255), (255, 255, 0)]
def bgr888(name):
    d = load(name); pitch = 5760
    print('[%s] 8 條色帶，記憶體 3 bytes/pixel：' % name)
    for i in range(8):
        x = i * 240 + 120; o = 500 * pitch + x * 3
        print('  %-8s bytes=%s' % (BARS[i], d[o:o + 3].hex(' ')))

def rgb565(name):
    d = load(name); pitch = 3840
    print('[%s] 漸層，16-bit：' % name)
    for x, y in ((0, 0), (1919, 0), (0, 1079), (960, 540)):
        v = struct.unpack_from('<H', d, y * pitch + x * 2)[0]
        r, g, b = v >> 11, (v >> 5) & 63, v & 31; e = grad(x, y)
        print('  (%4d,%4d) raw=%04x → R5=%2d G6=%2d B5=%2d   預期 %s → %d/%d/%d' % (x, y, v, r, g, b, e, e[0] >> 3, e[1] >> 2, e[2] >> 3))

def nv12(name):
    d = load(name); pitch = 1920; h = 1080
    print('[%s] 8 條色帶的 Y/U/V：' % name)
    rows = []
    for i in range(8):
        x = i * 240 + 120; y = 500
        Y = d[y * pitch + x]; uvo = h * pitch + (y // 2) * pitch + (x // 2) * 2
        U, V = d[uvo], d[uvo + 1]
        rows.append((BARRGB[i], Y, U, V))
        print('  %-8s RGB=%-15s Y=%3d U=%3d V=%3d' % (BARS[i], BARRGB[i], Y, U, V))
    import itertools
    # 用 R/G/B 三原色反推 Y 係數與偏移
    k = rows[0][1]; kr = (rows[2][1] - k) / 255; kg = (rows[3][1] - k) / 255; kb = (rows[4][1] - k) / 255
    print('  反推：Y = %.4f R + %.4f G + %.4f B + %d   （白色 Y=%d）' % (kr, kg, kb, k, rows[1][1]))
    for nm, (a, b, c) in (('BT.601', (0.299, 0.587, 0.114)), ('BT.709', (0.2126, 0.7152, 0.0722)), ('BT.2020', (0.2627, 0.6780, 0.0593))):
        for rng, sc, off in (('full', 1, 0), ('limited', 219 / 255, 16)):
            print('    %-7s %-7s → %.4f %.4f %.4f +%d' % (nm, rng, a * sc, b * sc, c * sc, off))

F = {'compose_cov': compose, 'compose_pre': compose, 'upscale2x': upscale, 'wb_xhalf': xhalf, 'wb_yhalf': yhalf,
     'fmt_bgr888': bgr888, 'fmt_rgb565': rgb565, 'fmt_nv12': nv12}
for n in sys.argv[1:]:
    try: F[n](n)
    except FileNotFoundError: print('[%s] 沒有檔案' % n)
