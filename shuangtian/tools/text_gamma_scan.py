"""gamma 扫描汇总：同一把尺子（线性光口径）比较 g ∈ {1.0, 1.2, 1.4, 1.6, 1.8, 2.0, 2.2}。

对每个 g：
  - 与浏览器的 **线性口径** ink / solid 偏差（几何+合成双重可比）
  - 与浏览器的 **code 空间** mean|Δ|（text_preblend_fit.py 的老口径，供衔接）
  - 逐带明细
输出一张可直接贴进 DESIGN.md 的表。

用法：python text_gamma_scan.py <batches_dir>   # 目录下有 g1.0/ g1.4/ ... 各含 ab-lcdF-normal.png
"""
import glob
import os
import sys

import numpy as np
from PIL import Image

FG = np.array([0x0F, 0x17, 0x2A], dtype=float)
BG = np.array([0xFF, 0xFF, 0xFF], dtype=float)
BANDS = [("14px CJK", 0, 45), ("13.5px 拉丁", 45, 87), ("12px 路径", 87, 117),
         ("13.5px 等宽", 117, 150)]


def srgb_to_linear(v):
    v = np.clip(v, 0.0, 1.0)
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def luminance(rgb):
    lin = srgb_to_linear(rgb / 255.0)
    return 0.2126 * lin[..., 0] + 0.7152 * lin[..., 1] + 0.0722 * lin[..., 2]


Y_BG = float(luminance(BG))
Y_FG = float(luminance(FG))


def linear_cov(rgb):
    return np.clip((Y_BG - luminance(rgb)) / (Y_BG - Y_FG), 0, 1)


def code_cov(rgb):
    d = BG - FG
    return np.clip(((BG - rgb) @ d) / float(d @ d), 0.0, 1.0)


def best_shift(a, b, span=6):
    best = (0, 0, 1e9)
    for dy in range(-span, span + 1):
        for dx in range(-span, span + 1):
            s = np.abs(a - np.roll(np.roll(b, dy, axis=0), dx, axis=1)).mean()
            if s < best[2]:
                best = (dx, dy, s)
    return best[0], best[1]


def load(p):
    return np.asarray(Image.open(p).convert("RGB")).astype(float)


if __name__ == "__main__":
    root = sys.argv[1]
    browser = load(os.path.join(root, "ab", "browser.png"))
    dirs = sorted(d for d in glob.glob(os.path.join(root, "g*")) if os.path.isdir(d))
    rows = []
    for d in dirs:
        tag = os.path.basename(d)
        ours = load(os.path.join(d, "ab-lcdF-normal.png"))
        h = min(browser.shape[0], ours.shape[0])
        w = min(browser.shape[1], ours.shape[1])
        br, ou = browser[:h, :w], ours[:h, :w]
        per_band = []
        for name, y0, y1 in BANDS:
            b, o = br[y0:y1], ou[y0:y1]
            bc, oc = linear_cov(b), linear_cov(o)
            dx, dy = best_shift(oc, bc)
            bs = np.roll(b, (dy, dx), axis=(0, 1))
            bl, ol = linear_cov(bs), linear_cov(o)
            mask = (ol > 0.02) | (bl > 0.02)
            ink_dev = (ol[mask].sum() / max(1e-9, bl[mask].sum()) - 1.0) * 100.0
            solid_b = int((bl > 0.85).sum())
            solid_o = int((ol > 0.85).sum())
            solid_dev = (solid_o / max(1, solid_b) - 1.0) * 100.0
            code_delta = float(np.abs(code_cov(o) - code_cov(bs))[mask].mean())
            per_band.append((name, ink_dev, solid_dev, code_delta))
        rows.append((tag, per_band))

    print(f"{'batch':8s}" + "".join(f"{n:>22s}" for n, _, _, _ in rows[0][1]))
    print(f"{'':8s}" + "".join(f"{'ink% / solid% / codeΔ':>22s}" for _ in rows[0][1]))
    for tag, per_band in rows:
        cells = "".join(f"{ink:+7.1f} {sol:+7.1f} {cd:7.1f}" for _, ink, sol, cd in per_band)
        print(f"{tag:8s}{cells}")
    print("\n汇总结论（4 带平均）：")
    for tag, per_band in rows:
        inks = [p[1] for p in per_band]
        sols = [p[2] for p in per_band]
        codes = [p[3] for p in per_band]
        print(f"  {tag:8s} ink 平均 {np.mean(inks):+6.1f}% (|·| 平均 {np.mean(np.abs(inks)):5.1f}%)  "
              f"solid 平均 {np.mean(sols):+6.1f}% (|·| {np.mean(np.abs(sols)):5.1f}%)  "
              f"codeΔ 平均 {np.mean(codes):6.2f}")
