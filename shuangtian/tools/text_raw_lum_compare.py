"""**朴素像素口径**复核：不比覆盖率（任何反解都有口径假设），直接比原始像素亮度。

问题（2026-10-04 用户反馈）：覆盖率 gamma 关掉（γ=1）的代码编辑器**看着更实**，
γ=2.2 看着发灰、发虚——与我上一轮「与浏览器线性光墨量偏差从 +13.8% 降到 +3.3%」的结论相反。
所以要么我的参照（无头 Edge）不代表真实参照，要么我的口径掩盖了观感。

三个最朴素的量（都不经覆盖率反解，接近眼睛看到的）：
  mean_lum    文字区域的**平均亮度**（越低越黑/越实）
  dark_ratio  亮度 < 50% 的像素占比（"多少像素是黑的"）
  mid_ratio   亮度 50%~90% 的像素占比（**过渡带**，越高越"灰"）

用法：python tools/text_raw_lum_compare.py <图A> <图B> [...]
     按标签聚合，逐带输出三组数（可选 --band y0 y1）。
"""
import sys

import numpy as np
from PIL import Image


def luminance(rgb):
    return 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]


def report(path, box=None):
    rgb = np.asarray(Image.open(path).convert("RGB")).astype(float)
    if box is not None:
        x0, y0, x1, y1 = box
        rgb = rgb[y0:y1, x0:x1]
    lum = luminance(rgb) / 255.0
    ink = lum < 0.95  # 只有背景（白）之外的像素才参与
    if ink.sum() == 0:
        return None
    values = lum[ink]
    return dict(
        n=int(ink.sum()),
        mean_lum=float(values.mean()),
        dark_ratio=float((values < 0.5).mean()),
        mid_ratio=float(((values >= 0.5) & (values < 0.90)).mean()),
        very_dark=float((values < 0.25).mean()),
    )


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    print(f"{'file':34s} {'墨像素':>8s} {'平均亮度':>9s} {'<50%':>8s} {'<25%':>8s} {'50~90%':>8s}")
    for path in args:
        r = report(path)
        if r is None:
            print(f"{path:34s} （无墨像素）")
            continue
        print(f"{path.split('/')[-1].split(chr(92))[-1]:34s} {r['n']:8d} {r['mean_lum']:9.3f} "
              f"{r['dark_ratio']*100:7.1f}% {r['very_dark']*100:7.1f}% {r['mid_ratio']*100:7.1f}%")
    print("\n判读：平均亮度越低 = 字越实；<50% 越高 = 越多的墨是实的；")
    print("      50~90% 越高 = 过渡带越宽（越'灰'/'虚'）——这是本次要分辨的量。")
