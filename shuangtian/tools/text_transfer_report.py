"""覆盖率**传递曲线**拟合（仅验证用）。

问题：霜天与浏览器在同一段文本上的差异，到底是「位置/形状不同」还是
「同一份覆盖率被映射成了不同的亮度」？后者是纯色调映射问题（gamma / 对比度），
前者才是几何问题——两者的修法完全不同，先把它们分开。

做法：把两张图按最优位移对齐后，取两者覆盖率都在 0.02..0.98 的像素，
按霜天覆盖率分箱，看浏览器覆盖率的中位数。
  · 若中位数 ≈ 我们的值        → 无色调差（差异全来自几何）
  · 若呈幂律 c_b ≈ c_s^g       → 浏览器施加了 gamma
  · 若呈线性放大 c_b ≈ k·c_s   → 浏览器施加了对比度增强
  · 若在两端饱和、中间拉开     → 典型 S 曲线（ClearType 的对比度增强）
"""

import sys

import numpy as np
from PIL import Image

FG = np.array([0x0F, 0x17, 0x2A], dtype=float)
BG = np.array([0xFF, 0xFF, 0xFF], dtype=float)


def cov_of(path):
    a = np.asarray(Image.open(path).convert("RGB")).astype(float)
    d = BG - FG
    return np.clip(((BG - a) @ d) / float(d @ d), 0.0, 1.0)


def best_shift(a, b, span=6):
    best = (0, 0, 1e9)
    for dy in range(-span, span + 1):
        for dx in range(-span, span + 1):
            score = np.abs(a - np.roll(np.roll(b, dy, axis=0), dx, axis=1)).mean()
            if score < best[2]:
                best = (dx, dy, score)
    return best


def transfer(ours, browser, tag, edges=None):
    if edges is None:
        edges = np.arange(0.0, 1.01, 0.1)
    print(f"\n  传递曲线 {tag}（按霜天覆盖率分箱 → 浏览器覆盖率中位数）")
    print("    霜天区间        n     霜天均值   浏览器中位数   比值")
    xs, ys, ws = [], [], []
    for lo, hi in zip(edges[:-1], edges[1:]):
        mask = (ours >= lo) & (ours < hi) & (browser >= 0.0)
        n = int(mask.sum())
        if n < 100:
            continue
        x = float(ours[mask].mean())
        y = float(np.median(browser[mask]))
        print(f"    [{lo:.2f},{hi:.2f})  {n:7d}   {x:.3f}      {y:.3f}       "
              f"{y / x if x > 1e-6 else float('nan'):.3f}")
        xs.append(x)
        ys.append(y)
        ws.append(n)
    xs, ys, ws = map(np.array, (xs, ys, ws))
    if len(xs) < 3:
        return
    # 幂律拟合 log y = g log x + c
    good = (xs > 0.05) & (ys > 0.02)
    if good.sum() >= 3:
        g, c = np.polyfit(np.log(xs[good]), np.log(ys[good]), 1)
        print(f"    → 幂律拟合：浏览器 ≈ {np.exp(c):.3f} × 霜天^{g:.3f}  "
              f"（g>1 表示浏览器把中间调压暗 = gamma 增强）")
    # 线性拟合
    k, b = np.polyfit(xs, ys, 1)
    print(f"    → 线性拟合：浏览器 ≈ {k:.3f} × 霜天 {b:+.3f}（斜率>1 = 对比度增强）")


if __name__ == "__main__":
    browser = cov_of(sys.argv[1])
    ours = cov_of(sys.argv[2])
    rows = [(0, 58, "26px 标题"), (58, 100, "13.5px 正文"), (100, 138, "12.5px 小字")]
    for y0, y1, name in rows:
        a = ours[y0:y1]
        b = browser[y0:y1]
        dx, dy, _ = best_shift(a, b)
        b = np.roll(np.roll(b, dy, axis=0), dx, axis=1)
        transfer(a, b, f"{name} (对齐 dx={dx} dy={dy})")
