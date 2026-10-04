"""真实截图里的**逐区域**锐度对照（按各区域自己的前景色归一）。

为什么不是简单按正文色算覆盖率：同一屏里同时存在正文色 #0F172A、次级色 #56647C、
弱化色 #66768C 与语法高亮的多种颜色。统一按正文色归一，会把「设计上就该浅」
误判成「笔画没到满黑」——2026-10-04 的两次错口径都是这么来的。

做法：
  1. 按行投影自动切出**文字带**（连续的非背景行）；
  2. 每个带内取「最暗像素的众数颜色」作为该带的**前景基准**；
  3. 按 `(bg - lum)/(bg - fg) 归一后统计 solid / mid / half / ink。

于是「同一个数」在不同颜色、不同字号上可以直接横比。
"""

import sys

import numpy as np
from PIL import Image


def analyze(path, min_height=8):
    a = np.asarray(Image.open(path).convert("RGB")).astype(float)
    lum = a.mean(axis=2)
    height, width = lum.shape
    background = float(np.bincount(np.round(lum).astype(int).ravel(), minlength=256).argmax())

    rows = (lum < background - 6).sum(axis=1)
    bands = []
    inside = False
    start = 0
    for y in range(height):
        if rows[y] > 0 and not inside:
            start, inside = y, True
        elif rows[y] == 0 and inside:
            if y - start >= min_height:
                bands.append((start, y))
            inside = False
    if inside and height - start >= min_height:
        bands.append((start, height))

    print(f"截图 {path}  背景≈{background:.0f}  文字带 {len(bands)} 条\n")
    print(f"  {'带(y0-y1)':>14s} {'前景':>4s} {'墨迹px':>8s} {'solid':>7s} {'mid':>7s} "
          f"{'half':>6s} {'mid/solid':>10s} {'ink':>9s}")
    for y0, y1 in bands:
        seg = a[y0:y1]
        sl = lum[y0:y1]
        # 前景 = 该带内最暗那 2% 像素的**众数**（代表「这一带最实的笔画颜色」）
        cutoff = np.percentile(sl, 2.0)
        dark = sl[sl <= cutoff]
        if dark.size == 0:
            continue
        fg = float(np.bincount(np.round(dark).astype(int).ravel(), minlength=256).argmax())
        if background - fg < 8.0:
            continue
        cov = np.clip((background - sl) / (background - fg), 0.0, 1.0)
        solid = int((cov > 0.85).sum())
        mid = int(((cov > 0.15) & (cov < 0.85)).sum())
        half = int(((cov > 0.45) & (cov < 0.55)).sum())
        ratio = (mid / solid) if solid else -1.0
        print(f"  {y0:6d}-{y1:<7d} {fg:4.0f} {solid + mid:8d} {solid:7d} {mid:7d} "
              f"{half:6d} {ratio:10.3f} {cov.sum():9.1f}")


if __name__ == "__main__":
    analyze(sys.argv[1])
