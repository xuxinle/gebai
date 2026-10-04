"""从**整屏截图**里裁出对照页文字区，并用朴素口径与霜天渲染直接比。

用法：python tools/text_reference_crop.py <整屏png> <霜天png> [dx dy w h]
缺省自动在整屏图左上区域找“最上面那些深色行”作为文字区。
"""
import sys

import numpy as np
from PIL import Image


def luminance(rgb):
    return (0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]) / 255.0


def naive(rgb, box=None):
    if box is not None:
        x0, y0, x1, y1 = box
        rgb = rgb[y0:y1, x0:x1]
    lum = luminance(rgb)
    ink = lum < 0.95
    if ink.sum() == 0:
        return None
    v = lum[ink]
    return dict(n=int(ink.sum()), mean=float(v.mean()),
                dark=float((v < 0.5).mean()), vdark=float((v < 0.25).mean()),
                band=float(((v >= 0.5) & (v < 0.90)).mean()))


if __name__ == "__main__":
    full = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(float)
    ours = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(float)
    if len(sys.argv) >= 7:
        dx, dy, w, h = (int(a) for a in sys.argv[3:7])
    else:
        # 自动：在白底大区域里找最上面的连续深色行（跳过浏览器标题栏）
        lum = luminance(full)
        rows = (lum < 0.6).sum(axis=1)
        hit = np.where(rows > 0)[0]
        hit = hit[hit > 30]                      # 跳过窗口边框
        y0 = int(hit.min()) - 6
        y1 = int(hit.min()) + 200
        cols = (luminance(full[y0:y1]) < 0.6).sum(axis=0)
        chit = np.where(cols > 0)[0]
        dx, dy, w, h = int(chit.min()) - 8, y0, int(chit.max() - chit.min()) + 16, y1 - y0
    print(f"参照区：x={dx} y={dy} w={w} h={h}（整屏 {full.shape[1]}x{full.shape[0]}）")
    ref = naive(full, (dx, dy, dx + w, dy + h))
    a = naive(ours) if ours.shape[0] <= h + 40 else naive(ours, (0, 0, ours.shape[1], min(ours.shape[0], h)))
    print(f"{'参照(真机浏览器)':22s} 墨像素={ref['n']:6d} 平均亮度={ref['mean']:.3f} "
          f"<50%={ref['dark']*100:5.1f}% <25%={ref['vdark']*100:5.1f}% 过渡带={ref['band']*100:5.1f}%")
    print(f"{'霜天':22s} 墨像素={a['n']:6d} 平均亮度={a['mean']:.3f} "
          f"<50%={a['dark']*100:5.1f}% <25%={a['vdark']*100:5.1f}% 过渡带={a['band']*100:5.1f}%")
    print("\n判读：参照的 <50%（实心）比霜天高 ⇒ 霜天偏**轻**，应往加墨方向；")
    print("      反之 ⇒ 霜天偏重。过渡带越宽越“灰/虚”。")
