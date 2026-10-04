"""逐行带比对：真机浏览器参照 vs 霜天渲染（朴素像素口径）。

用法：python tools/text_reference_bands.py <参照整屏png> <霜天png> <ref_x0> <ref_y0> [scale]
参照图的带位置按 4 行文本的行距推算（对照页行距 28 CSS px × scale）。
"""
import sys

import numpy as np
from PIL import Image


def lum(a):
    return (0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]) / 255.0


def stat(a, box):
    x0, y0, x1, y1 = box
    a = a[y0:y1, x0:x1]
    l = lum(a)
    ink = l < 0.95
    if ink.sum() == 0:
        return (0, 0.0, 0.0, 0.0)
    v = l[ink]
    return (int(ink.sum()), float(v.mean()), float((v < 0.5).mean() * 100),
            float(((v >= 0.5) & (v < 0.9)).mean() * 100))


if __name__ == "__main__":
    ref = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(float)
    our = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(float)
    rx, ry = int(sys.argv[3]), int(sys.argv[4])
    scale = float(sys.argv[5]) if len(sys.argv) > 5 else 1.5
    step = 28.0 * scale
    width = int(240 * scale)
    height = int(24 * scale)
    print(f"参照图 {ref.shape[1]}x{ref.shape[0]}  霜天图 {our.shape[1]}x{our.shape[0]}  "
          f"行距 {step:.0f}px")
    print(f"{'带':14s} | {'参照 n':>7s} {'平均亮度':>8s} {'<50%':>7s} {'过渡带':>7s} "
          f"| {'霜天 n':>7s} {'平均亮度':>8s} {'<50%':>7s} {'过渡带':>7s}")
    for index in range(4):
        y0 = int(ry - 8 * scale + index * step)
        r = stat(ref, (rx, y0, rx + width, y0 + height))
        o = stat(our, (0, max(0, y0 - int(ry)), our.shape[1],
                       min(our.shape[0], max(0, y0 - int(ry)) + height)))
        print(f"{'行 ' + str(index + 1):14s} | {r[0]:7d} {r[1]:8.3f} {r[2]:6.1f}% {r[3]:6.1f}% "
              f"| {o[0]:7d} {o[1]:8.3f} {o[2]:6.1f}% {o[3]:6.1f}%")
    print("\n判读：参照 <50% 比霜天高 ⇒ 霜天偏轻（该加墨）；过渡带比参照宽 ⇒ 偏虚。")
