"""线性光口径的逐行 A/B：把「几何差」与「合成空间差」分开。

为什么必须分两个口径：
  现有 `text_ab_report.py` 用 **code 空间投影** 当覆盖率（v 直接线性映射）。
  但屏幕是 sRGB 非线性：真实覆盖率 a 的像素，其 code 值应是 `linear_to_srgb(1-a)`
  （黑字白底）。于是同一个真实覆盖率在两个合成空间下量出来的"覆盖率"完全不同——
  直接比 code 空间数字会把「合成空间不同」误读成「笔画更粗」。

本脚本同时给：
  code  口径：α = (BG−v)/(BG−FG)（= 现有报告，仅供与历史数字衔接）
  linear 口径：α = (Y_BG−Y_v)/(Y_BG−Y_FG)，Y = srgb→linear 亮度（**真实几何覆盖率**）
并逐行带给出 peak/solid/mid/half/ink ——linear 口径下两边可比，差值才是几何差。

用法：python text_linear_report.py <browser.png> <ours.png> [--bands 行带]
"""
import sys

import numpy as np
from PIL import Image

FG = np.array([0x0F, 0x17, 0x2A], dtype=float)
BG = np.array([0xFF, 0xFF, 0xFF], dtype=float)


def srgb_to_linear(v):
    v = np.clip(v, 0.0, 1.0)
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def luminance(rgb):
    lin = srgb_to_linear(rgb / 255.0)
    return 0.2126 * lin[..., 0] + 0.7152 * lin[..., 1] + 0.0722 * lin[..., 2]


def code_coverage(rgb):
    d = BG - FG
    return np.clip(((BG - rgb) @ d) / float(d @ d), 0.0, 1.0)


def linear_coverage(rgb):
    y = luminance(rgb)
    y_bg = float(luminance(BG))
    y_fg = float(luminance(FG))
    return np.clip((y_bg - y) / (y_bg - y_fg), 0.0, 1.0)


def metrics(cov):
    solid = int((cov > 0.85).sum())
    mid = int(((cov > 0.15) & (cov < 0.85)).sum())
    half = int(((cov > 0.45) & (cov < 0.55)).sum())
    ratio = (mid / solid) if solid else -1.0
    return dict(peak=float(cov.max()), solid=solid, mid=mid, half=half,
                ratio=ratio, ink=float(cov.sum()))


def best_shift(a, b, span=6):
    best = (0, 0, 1e9)
    for dy in range(-span, span + 1):
        for dx in range(-span, span + 1):
            score = np.abs(a - np.roll(np.roll(b, dy, axis=0), dx, axis=1)).mean()
            if score < best[2]:
                best = (dx, dy, score)
    return best[0], best[1]


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(float)


def report(name, y0, y1, br, ours):
    b = br[y0:y1]
    o = ours[y0:y1]
    if b.size == 0 or o.size == 0:
        return
    # 用线性口径对齐位移（几何对齐，与合成空间无关）
    bc = linear_coverage(b)
    oc = linear_coverage(o)
    dx, dy = best_shift(oc, bc)
    b_shift = np.roll(b, (dy, dx), axis=(0, 1))
    print(f"\n=== {name}  行带[{y0}:{y1}] 对齐 dx={dx} dy={dy} ===")
    for tag, cov_fn in (("code ", code_coverage), ("linear", linear_coverage)):
        m_b = metrics(cov_fn(b_shift))
        m_o = metrics(cov_fn(o))
        print(f"  [{tag}] {'side':8s} peak={m_b['peak']:.3f} solid={m_b['solid']:5d} "
              f"mid={m_b['mid']:5d} half={m_b['half']:4d} mid/solid={m_b['ratio']:6.3f} ink={m_b['ink']:9.1f}")
        print(f"  [{tag}] {'霜天':8s} peak={m_o['peak']:.3f} solid={m_o['solid']:5d} "
              f"mid={m_o['mid']:5d} half={m_o['half']:4d} mid/solid={m_o['ratio']:6.3f} ink={m_o['ink']:9.1f}")
        ink_delta = (m_o['ink'] / m_b['ink'] - 1.0) * 100.0 if m_b['ink'] else 0.0
        print(f"  [{tag}] 墨量差 {ink_delta:+.1f}%   solid 差 {(m_o['solid'] / max(1, m_b['solid']) - 1.0) * 100:+.1f}%")
    # 传递曲线：按霜天线性覆盖率分箱，看浏览器的线性覆盖率中位数
    print("  传递曲线（霜天线性覆盖率分箱 → 浏览器线性覆盖率均值）：")
    oc_l = linear_coverage(o)
    bc_l = linear_coverage(b_shift)
    edges = [0.0, 0.05, 0.15, 0.3, 0.45, 0.55, 0.7, 0.85, 0.95, 1.01]
    for i in range(len(edges) - 1):
        lo, hi = edges[i], edges[i + 1]
        mask = (oc_l >= lo) & (oc_l < hi)
        if mask.sum() < 20:
            continue
        print(f"    ours {lo:.2f}~{hi:.2f}: n={int(mask.sum()):5d}  "
              f"browser 均值={bc_l[mask].mean():.3f}  ours 均值={oc_l[mask].mean():.3f}")


if __name__ == "__main__":
    br = load(sys.argv[1])
    ours = load(sys.argv[2])
    print(f"browser {br.shape[1]}x{br.shape[0]}   ours {ours.shape[1]}x{ours.shape[0]}")
    h = min(br.shape[0], ours.shape[0])
    w = min(br.shape[1], ours.shape[1])
    br = br[:h, :w]
    ours = ours[:h, :w]
    bands = [("14px CJK 资源管理器…", 0, 45), ("13.5px 拉丁 Settings…", 45, 87),
             ("12px 路径 src/raster…", 87, 117), ("13.5px 等宽 const auto…", 117, 150)]
    for name, y0, y1 in bands:
        if y0 >= h:
            continue
        report(name, y0, min(y1, h), br, ours)
