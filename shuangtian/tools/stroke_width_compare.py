"""量**笔画宽度**（物理像素）——比墨量比更能说明"偏粗"。

墨量比受行内字符数/间距影响；笔画宽度是"这根竖笔有多宽"，是"粗"的直接量。

做法：沿水平扫描线找覆盖率跨越 0.5 的**上升沿→下降沿**对，其间距即该处笔画宽度；
只统计 1~6 物理像素的段（排除整块实心与噪声），取中位数与均值。

用法：python tools/stroke_width_compare.py <A.png> <B.png> <boxes.json> [行索引...]
"""

import json
import statistics
import sys

import numpy as np
from PIL import Image

TEXT = (0x0F, 0x17, 0x2A)
BG = np.array([255.0, 255.0, 255.0])


def coverage(img, y0, y1, fg=TEXT):
    sub = img[y0:y1, :, :]
    d = BG - np.array(fg, dtype=float)
    return np.clip(((BG - sub) @ d) / float(d @ d), 0.0, 1.0)


def strokes(cov, lo=1.0, hi=6.0):
    """每行扫描线上 0.5 阈值的实心段宽度（物理像素）。"""
    out = []
    for row in cov:
        solid = row > 0.5
        x = 0
        n = len(solid)
        while x < n:
            if not solid[x]:
                x += 1
                continue
            start = x
            while x < n and solid[x]:
                x += 1
            w = x - start
            if lo <= w <= hi:
                out.append(w)
    return out


def main() -> int:
    a = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(float)
    b = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(float)
    boxes = json.loads(open(sys.argv[3]).read())
    rows = [int(v) for v in sys.argv[4:]] or list(range(len(boxes)))
    print(f"{'行':<6}{'霜天 均/中位':>18}{'浏览器 均/中位':>18}{'宽度比':>9}  样本数")
    print("-" * 70)
    for i in rows:
        y0, y1 = boxes[i][1], boxes[i][2]
        sa = strokes(coverage(a, y0, y1))
        sb = strokes(coverage(b, y0, y1))
        if not sa or not sb:
            continue
        ma, mb = statistics.mean(sa), statistics.mean(sb)
        print(f"r{i:<5}{ma:>9.2f}/{statistics.median(sa):<8.2f}"
              f"{mb:>9.2f}/{statistics.median(sb):<8.2f}{ma / mb:>9.3f}  {len(sa)}/{len(sb)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
