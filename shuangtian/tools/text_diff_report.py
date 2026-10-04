"""对齐后逐像素差异定位（仅验证用）。

先把两张图按**字形包围盒**对齐（而不是按页面坐标），再分档比较——
页面坐标对不齐时，「差在哪」完全被错位量淹没（实测 4 物理像素的基线差
会把所有过渡带指标污染成噪声）。对齐用整幅互相关求最优位移，
比较只在**字符实际占据的区域**内进行，背景不计入。
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


def bin_report(a, b, tag):
    ink = (a > 0.03) | (b > 0.03)
    diff = np.abs(a - b)[ink]
    print(f"  {tag:14s} 墨迹像素={int(ink.sum()):6d} mean|Δ|={diff.mean():.4f} "
          f"p95|Δ|={np.percentile(diff, 95):.3f}")
    bins = [(0.85, 1.01, "实心 >0.85"), (0.55, 0.85, "深过渡"), (0.45, 0.55, "半覆盖"),
            (0.15, 0.45, "浅过渡"), (0.03, 0.15, "极浅")]
    for lo, hi, name in bins:
        mask = ink & (((a >= lo) & (a < hi)) | ((b >= lo) & (b < hi)))
        if mask.sum() < 20:
            print(f"    {name:10s} n={int(mask.sum()):5d}（样本过少，略）")
            continue
        print(f"    {name:10s} n={int(mask.sum()):5d} 霜天={a[mask].mean():.3f} "
              f"浏览器={b[mask].mean():.3f} 差={a[mask].mean() - b[mask].mean():+.4f}")


if __name__ == "__main__":
    browser = cov_of(sys.argv[1])
    ours = cov_of(sys.argv[2])
    rows = [(0, 58, "26px 标题"), (58, 100, "13.5px 正文"),
            (100, 138, "12.5px 小字"), (138, 180, "11px 脚注")]
    for y0, y1, name in rows:
        a = ours[y0:y1]
        b = browser[y0:y1]
        dx, dy, score = best_shift(a, b)
        a_al = a
        b_al = np.roll(np.roll(b, dy, axis=0), dx, axis=1)
        print(f"\n=== {name}（对齐 dx={dx} dy={dy}）===")
        bin_report(a_al, b_al, name)
