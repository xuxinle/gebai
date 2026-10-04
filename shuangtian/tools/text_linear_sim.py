"""模拟「线性空间合成」修复的效果——**在改 C++ 之前先证明它有效**。

原理：
  霜天当前在 **sRGB code 空间**做 alpha 混合：out_code = fg·a + bg·(1−a)（黑字白底 → 255·(1−a)）。
  由此可以从霜天 PNG **反解出它当时用的覆盖率** a = (255 − code) / 255。
  线性正确的合成应满足：Y_out = Y_fg·a + Y_bg·(1−a)（Y = sRGB→线性），
  即 code_out = linear_to_srgb(1 − a)（黑字白底）。

  把反解出的 a 按线性口径重新映射，就得到「改成线性合成后」的模拟像素——
  与浏览器 PNG 逐像素比，即可判断该修复能否消掉墨量/实心像素的系统性偏差。

判据：
  linear 墨量差（霜天/browser − 1）：现状 vs 模拟后
  传递曲线：模拟后每个覆盖率分箱的 browser 均值是否与霜天对齐

用法：python text_linear_sim.py <browser.png> <ours.png>
"""
import sys

import numpy as np
from PIL import Image

FG = np.array([0x0F, 0x17, 0x2A], dtype=float)
BG = np.array([0xFF, 0xFF, 0xFF], dtype=float)


def srgb_to_linear(v):
    v = np.clip(v, 0.0, 1.0)
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(v):
    v = np.clip(v, 0.0, 1.0)
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * np.power(v, 1.0 / 2.4) - 0.055)


def luminance(rgb):
    lin = srgb_to_linear(rgb / 255.0)
    return 0.2126 * lin[..., 0] + 0.7152 * lin[..., 1] + 0.0722 * lin[..., 2]


def linear_coverage(rgb):
    y = luminance(rgb)
    return np.clip((float(luminance(BG)) - y) / (float(luminance(BG)) - float(luminance(FG))), 0, 1)


def metrics(cov):
    solid = int((cov > 0.85).sum())
    mid = int(((cov > 0.15) & (cov < 0.85)).sum())
    ratio = (mid / solid) if solid else -1.0
    return dict(solid=solid, mid=mid, ratio=ratio, ink=float(cov.sum()))


def best_shift(a, b, span=6):
    best = (0, 0, 1e9)
    for dy in range(-span, span + 1):
        for dx in range(-span, span + 1):
            score = np.abs(a - np.roll(np.roll(b, dy, axis=0), dx, axis=1)).mean()
            if score < best[2]:
                best = (dx, dy, score)
    return best[0], best[1]


def simulate_linear(rgb):
    """把「sRGB code 空间混合」的渲染结果，重映射为「线性空间混合」的结果。

    黑字白底：code = 255·(1−a) → a = 1 − code/255；
    线性正确：code' = 255·linear_to_srgb(1 − a) = 255·linear_to_srgb(code/255)。
    即：对**每个通道**做一次 sRGB→线性→**不**变……不，是 code' = srgb(linear(code))？
    —— 不：a 是逐通道的覆盖率反解，code'=srgb(Y_out)，Y_out = 1−a = code/255（线性值）。
    所以 code' = 255·linear_to_srgb(code/255) —— 一步 sRGB 逆变换外的正向变换。
    """
    return 255.0 * linear_to_srgb(rgb / 255.0)


if __name__ == "__main__":
    br = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(float)
    ours = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(float)
    h = min(br.shape[0], ours.shape[0])
    w = min(br.shape[1], ours.shape[1])
    br, ours = br[:h, :w], ours[:h, :w]
    sim = simulate_linear(ours)

    bands = [("14px CJK", 0, 45), ("13.5px 拉丁", 45, 87), ("12px 路径", 87, 117),
             ("13.5px 等宽", 117, 150)]
    for name, y0, y1 in bands:
        if y0 >= h:
            continue
        b = br[y0:min(y1, h)]
        o = ours[y0:min(y1, h)]
        s = sim[y0:min(y1, h)]
        oc, bc = linear_coverage(o), linear_coverage(b)
        dx, dy = best_shift(oc, bc)
        b = np.roll(b, (dy, dx), axis=(0, 1))
        bc = linear_coverage(b)
        print(f"\n=== {name} 对齐 dx={dx} dy={dy} ===")
        for tag, img in (("现状(code 空间混合)", o), ("模拟(线性空间混合)", s)):
            m_b, m_o = metrics(bc), metrics(linear_coverage(img))
            print(f"  {tag:22s} solid={m_o['solid']:5d}(browser {m_b['solid']:5d}, "
                  f"{(m_o['solid']/max(1,m_b['solid'])-1)*100:+6.1f}%)  "
                  f"mid={m_o['mid']:5d}(browser {m_b['mid']:5d})  "
                  f"ink={m_o['ink']:8.1f}(browser {m_b['ink']:8.1f}, "
                  f"{(m_o['ink']/m_b['ink']-1)*100:+6.1f}%)")
        # 传递曲线对齐度（模拟后）
        sc = linear_coverage(s)
        edges = [0.05, 0.15, 0.3, 0.45, 0.55, 0.7, 0.85, 0.95, 1.01]
        print("    分箱（模拟后）      n   browser均值  ours均值")
        for i in range(len(edges) - 1):
            lo, hi = edges[i], edges[i + 1]
            mask = (sc >= lo) & (sc < hi)
            if mask.sum() < 20:
                continue
            print(f"    {lo:.2f}~{hi:.2f}  {int(mask.sum()):6d}   {bc[mask].mean():.3f}      {sc[mask].mean():.3f}")
