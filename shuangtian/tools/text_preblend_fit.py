"""**覆盖率预混合参数寻优**（仅验证用）——先取证、再改码。

ClearType / Skia 在 Windows LCD 上对字形覆盖率做一次「gamma + 对比度」预混合
（`SkMaskGamma`：先 `a^gamma` 线性化，再做 `(a - contrast)/(1 - contrast)` 对比度拉伸），
目的就是**把中间调压暗**，让笔画看起来更实、更锐。

本工具把候选预混合施加到**霜天现有覆盖率**上，与浏览器覆盖率逐像素比较，
按 mean|Δ| 排序。这样选参数有据可依，而不是照抄别人的常数：

    a' = clamp((a^gamma - contrast) / (1 - contrast), 0, 1)

identity 行是「不做任何预混合」的基线——若最优行明显优于它，
就说明浏览器确实施加了这类映射，且我们能定量复现它。
"""

import itertools
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


def preblend(cov, gamma, contrast):
    shaped = np.power(np.clip(cov, 0.0, 1.0), gamma)
    if contrast > 0.0:
        shaped = np.clip((shaped - contrast) / (1.0 - contrast), 0.0, 1.0)
    return shaped


if __name__ == "__main__":
    browser = cov_of(sys.argv[1])
    ours = cov_of(sys.argv[2])
    rows = [(0, 58, "26px 标题"), (58, 100, "13.5px 正文"), (100, 138, "12.5px 小字")]
    gammas = [1.0, 1.15, 1.3, 1.4, 1.55, 1.7, 1.9, 2.2]
    contrasts = [0.0, 0.1, 0.2, 0.3, 0.4, 0.5]
    for y0, y1, name in rows:
        a0 = ours[y0:y1]
        b0 = browser[y0:y1]
        dx, dy, _ = best_shift(a0, b0)
        b = np.roll(np.roll(b0, dy, axis=0), dx, axis=1)
        ink = (a0 > 0.03) | (b > 0.03)
        base = float(np.abs(a0 - b)[ink].mean())
        results = []
        for gamma, contrast in itertools.product(gammas, contrasts):
            shaped = preblend(a0, gamma, contrast)
            results.append((float(np.abs(shaped - b)[ink].mean()), gamma, contrast,
                            float(shaped[ink].sum())))
        results.sort()
        print(f"\n=== {name} (align dx={dx} dy={dy}, ink pixels {int(ink.sum())}) ===")
        print(f"  identity (no pre-blend)         mean|d|={base:.4f}  ink={a0[ink].sum():.0f}")
        print("  top 6 (gamma, contrast):")
        for score, gamma, contrast, ink_sum in results[:6]:
            gain = (base - score) / base * 100.0
            print(f"    gamma={gamma:<5} contrast={contrast:<4} mean|d|={score:.4f} "
                  f"(baseline -{gain:.1f}%) ink={ink_sum:.0f}")
        worst = results[-1]
        print(f"  worst: gamma={worst[1]} contrast={worst[2]} mean|d|={worst[0]:.4f}")
