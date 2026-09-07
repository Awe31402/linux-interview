#!/usr/bin/env python3
"""
exp05_tiling.py — 實驗 5.4：驗證分條規則

問題：一層被切成好幾條時，那些「高度」加起來為什麼不等於特徵圖高？

作法：解析 rkspy.c 產生的 log，對每一層算
        加總(片高) − (片數−1) × (k_h − s_y)
      看它是不是等於特徵圖高。

用法：
    # 先產生 log（在板子上）
    sudo env LD_PRELOAD=./rkspy.so RKSPY_LOG=/tmp/m1.log RKSPY_RAW=0 \
        ./exp05_run mobilenet_v1.rknn
    # 再分析（哪裡都可以跑）
    python3 exp05_tiling.py /tmp/m1.log 40

第二個參數是 subcore[0] 的 task 數量，從 log 的 SUBMIT #1 區塊讀：
    subcore[0]: start=0 number=40
任務陣列裡有三份 subcore 拷貝，只能取其中一份，否則會重複計算。
"""
import re, sys, collections

ROW = re.compile(
    r"^\s{2}(\d+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+"
    r"(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s*$")


def load(path, limit):
    """讀出第一個 SUBMIT、subcore[0] 範圍內的 CNA 分條。

    兩個關鍵處理：
      1. 沒有重寫暫存器的 task 沿用上一次的值 —— 硬體本來就是這樣
         （ping-pong 之下只寫有變動的暫存器，見 ch06 §8）
      2. 但**換了 op 就清空**。有些 op 完全沒碰 CNA 暫存器
         （純 DPU/PPU 運算），不該把上一層的值算到它頭上。
    """
    rows, in_first, nsub, cur_op, last = [], False, 0, None, {}
    for line in open(path):
        if "SUBMIT #" in line:
            nsub += 1
            in_first = (nsub == 1)
            cur_op, last = None, {}
            continue
        if not in_first:
            continue
        m = ROW.match(line)
        if not m:
            continue
        t, op = int(m.group(1)), int(m.group(2))
        if t >= limit:
            continue
        if op != cur_op:
            cur_op, last = op, {}
        vals = {}
        for key, raw in (("W", m.group(3)), ("H", m.group(4)),
                         ("kh", m.group(9)), ("sy", m.group(11))):
            if raw != "-":
                last[key] = int(raw)
            vals[key] = last.get(key)
        if vals["W"] is None or vals["kh"] is None:
            continue                      # 這個 op 從沒設過 CNA → 不是卷積
        rows.append(dict(task=t, op=op, **vals))
    return rows


def main(path, limit):
    groups = collections.OrderedDict()
    for r in load(path, limit):
        groups.setdefault((r["op"], r["W"]), []).append(r)

    print(f"{'op':>4} {'W':>5} {'k_h':>4} {'s_y':>4} {'片數':>4} "
          f"{'加總':>6} {'重疊':>5} {'扣掉重疊':>8}  判定")
    ok = bad = 0
    other = []
    for (op, W), rs in groups.items():
        n = len(rs)
        if n < 2:
            continue                      # 沒被切就沒得驗
        kh, sy = rs[0]["kh"], rs[0]["sy"]
        hs = [r["H"] for r in rs]
        if max(hs) >= W:
            other.append((op, W, hs))     # 每片都是整張高 → 不是按列切
            continue
        total = sum(hs)
        overlap = (n - 1) * (kh - sy)
        base = total - overlap
        good = (base == W)                # 方形輸入 → 特徵圖高應該等於寬
        ok += good
        bad += not good
        print(f"{op:>4} {W:>5} {kh:>4} {sy:>4} {n:>4} {total:>6} "
              f"{overlap:>5} {base:>8}  " + ("OK" if good else f"MISS (期望 {W})"))

    print(f"\n按列切的層：命中 {ok}/{ok + bad}")
    if other:
        print(f"另有 {len(other)} 層每片都是整張高，不是按列切（切法未解）：")
        for op, W, hs in other:
            print(f"  op{op} W={W} 片數={len(hs)} 片高都是 {hs[0]}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1], int(sys.argv[2]))
