#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""霜天 vs DirectWrite 的像素级对标量尺。

口径（沿用仓库既有约定，避免各写一份漂移）：
* **覆盖率投影**：每像素 RGB 投影到 [背景, 前景] 连线上得到 t∈[0,1]。
  亚像素渲染下 R/G/B 被不同强度点亮，逐通道做会重复计彩边，投影才是"这层墨有多厚"。
* **逐带配对**：两侧各自按行投影切带再按序配对。两侧基线口径不同源
  （DWrite 布局盒 ascent vs 霜天 ascender），按同一 y 带裁切会整行错位——
  TEXT_AB_REPORT §五-4 记过这个坑（把墨量比读成 0.41~0.65、结论方向相反）。
* **共享掩码**：面积/暗度两个量在同一掩码（并集）上算，否则面积差会被重复计入暗度。

指标（每个行带一组）：
  mass      总墨量 Σt（相对参照的比）
  solid     实心像素占比（t>0.85）比
  trans     过渡像素占比（0.15<t<0.85）比
  area      墨像素数（t>0.15）比
  dark      墨像素平均浓度比
  fringe    彩边强度（|R-B| 的加权均值，衡量亚像素滤波)
  sharp     边缘锐度（沿 x 的一阶差分在边缘处的均值）
  bw        墨迹包围盒宽高（几何未被画歪的直接判据）

用法：
  python tools/dw_ab_diff.py <dir> [--theme light|dark] [--ref natural|gdi|gray]
                              [--class filter] [--csv out.csv]
"""
import argparse
import csv
import json
import pathlib
import sys

import numpy as np
from PIL import Image

TOOLS = pathlib.Path(__file__).resolve().parent
DWAB = TOOLS.parent / "build" / "dwab"


def load_rgb(path):
    img = Image.open(path).convert("RGB")
    return np.asarray(img, dtype=np.float64)


def coverage(img, bg, fg):
    """把 RGB 投影到 bg→fg 连线，得到每像素覆盖率 t∈[0,1]（并行计算，R/G/B 等权）。"""
    d = fg - bg
    denom = float((d * d).sum())
    if denom < 1e-6:
        return np.zeros(img.shape[:2])
    t = ((img - bg) * d).sum(axis=2) / denom
    return np.clip(t, 0.0, 1.0)


def row_profile(t):
    """逐行墨量（覆盖率之和），用于切带。"""
    return t.sum(axis=1)


def find_bands(t, min_ink=1.0, gap_tol=2):
    """按行投影切出连续文字带；容忍 ≤gap_tol 行的空行（字的行内空隙）。"""
    prof = row_profile(t)
    rows = np.where(prof > min_ink)[0]
    if rows.size == 0:
        return []
    bands = []
    start = prev = rows[0]
    for y in rows[1:]:
        if y - prev > gap_tol:
            bands.append((int(start), int(prev) + 1))
            start = y
        prev = y
    bands.append((int(start), int(prev) + 1))
    return bands


def htrim(t, band, min_ink=0.5):
    """行带内水平紧裁到墨迹跨度，返回 (x0, x1)。"""
    y0, y1 = band
    col = t[y0:y1].sum(axis=0)
    cols = np.where(col > min_ink)[0]
    if cols.size == 0:
        return None
    return int(cols[0]), int(cols[-1]) + 1


def metrics(t, rgb, y0, y1, suffix=True):
    """单个行带（已按 x 裁好）的指标。t/rgb 都是同尺寸子图。"""
    if t.size == 0:
        return None
    ink = t > 0.15
    n_ink = int(ink.sum())
    if n_ink == 0:
        return None
    solid = float((t > 0.85).sum()) / n_ink
    trans = float(((t > 0.15) & (t < 0.85)).sum()) / n_ink
    # 彩边：墨像素上 |R-B| 的均值（ClearType 滤波越强，边缘彩边越明显）
    fringe = float(np.abs(rgb[..., 0] - rgb[..., 2])[ink].mean())
    # 锐度：x 方向一阶差分的 95 分位（边缘处才有大值；用分位避免被单像素尖峰带偏）
    gx = np.abs(np.diff(t, axis=1))
    sharp = float(np.percentile(gx[gx > 0.05], 95)) if (gx > 0.05).any() else 0.0
    ys, xs = np.where(ink)
    bw = int(xs.max() - xs.min() + 1)
    bh = int(y1 - y0)
    return {
        "mass": float(t.sum()),
        "n_ink": n_ink,
        "solid": solid,
        "trans": trans,
        "dark": float(t[ink].mean()),
        "fringe": fringe,
        "sharp": sharp,
        "bw": bw,
        "bh": bh,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", nargs="?", default=str(DWAB))
    ap.add_argument("--theme", default="light")
    ap.add_argument("--ref", default="natural", choices=["natural", "gdi", "gray"])
    ap.add_argument("--class", dest="cls", default=None)
    ap.add_argument("--stfile", default=None, help="霜天侧图片文件名（默认 st-<theme>.png）")
    ap.add_argument("--csv", default=None)
    args = ap.parse_args()
    d = pathlib.Path(args.dir)

    meta = json.loads((d / f"rows-{args.theme}.json").read_text(encoding="utf-8"))
    bg = np.array([int(meta["bg"][i:i + 2], 16) for i in (1, 3, 5)], dtype=np.float64)
    fg = np.array([int(meta["fg"][i:i + 2], 16) for i in (1, 3, 5)], dtype=np.float64)
    rows = meta["rows"]

    st_path = d / (args.stfile if args.stfile else f"st-{args.theme}.png")
    dw_path = d / f"dw-{args.ref}-{args.theme}.ppm"
    for p in (st_path, dw_path):
        if not p.exists():
            print(f"缺少 {p}", file=sys.stderr)
            return 1
    st = load_rgb(st_path)
    dw = load_rgb(dw_path)
    t_st = coverage(st, bg, fg)
    t_dw = coverage(dw, bg, fg)

    bs = find_bands(t_st)
    bd = find_bands(t_dw)
    if len(bs) != len(bd):
        print(f"[警告] 带数不等：霜天 {len(bs)} / DWrite {len(bd)}", file=sys.stderr)

    print(f"参照 = {args.ref}  主题 = {args.theme}  行表 = {len(rows)} 行  "
          f"带数 = {len(bs)}/{len(bd)}")
    print(f"背景 {tuple(int(v) for v in bg)}  前景 {tuple(int(v) for v in fg)}")
    print()
    hdr = (f"{'行':<12}{'类':>7}{'墨量比':>9}{'实心比':>8}{'过渡比':>8}{'面积比':>8}"
           f"{'浓度比':>8}{'彩边 霜/DW':>14}{'锐度 霜/DW':>14}{'宽 霜/DW':>12}")
    print(hdr)
    print("-" * len(hdr) * 1)

    out = []
    for i, (row, b1, b2) in enumerate(zip(rows, bs, bd)):
        if args.cls and row["class"] != args.cls:
            continue
        # 各自在自己的带内水平紧裁（两侧基线/侧承不同源，不能共用 x 范围）
        h1 = htrim(t_st, b1)
        h2 = htrim(t_dw, b2)
        if h1 is None or h2 is None:
            continue
        y0a, y1a = b1
        y0b, y1b = b2
        ta = t_st[y0a:y1a, h1[0]:h1[1]]
        tb = t_dw[y0b:y1b, h2[0]:h2[1]]
        ma = metrics(ta, st[y0a:y1a, h1[0]:h1[1]], 0, y1a - y0a)
        mb = metrics(tb, dw[y0b:y1b, h2[0]:h2[1]], 0, y1b - y0b)
        if ma is None or mb is None:
            continue
        rec = {
            "label": row["label"], "class": row["class"], "size": row["size"],
            "mass_r": ma["mass"] / mb["mass"] if mb["mass"] else 0.0,
            "solid_r": ma["solid"] / mb["solid"] if mb["solid"] else 0.0,
            "trans_r": ma["trans"] / mb["trans"] if mb["trans"] else 0.0,
            "area_r": ma["n_ink"] / mb["n_ink"] if mb["n_ink"] else 0.0,
            "dark_r": ma["dark"] / mb["dark"] if mb["dark"] else 0.0,
            "fringe_st": ma["fringe"], "fringe_dw": mb["fringe"],
            "sharp_st": ma["sharp"], "sharp_dw": mb["sharp"],
            "bw_st": ma["bw"], "bw_dw": mb["bw"],
            "bh_st": ma["bh"], "bh_dw": mb["bh"],
            "dy": y0a - y0b,
        }
        out.append(rec)
        print(f"{row['label']:<12}{row['class']:>7}{rec['mass_r']:>9.3f}{rec['solid_r']:>8.3f}"
              f"{rec['trans_r']:>8.3f}{rec['area_r']:>8.3f}{rec['dark_r']:>8.3f}"
              f"{ma['fringe']:>7.1f}/{mb['fringe']:<6.1f}"
              f"{ma['sharp']:>7.3f}/{mb['sharp']:<6.3f}"
              f"{ma['bw']:>5}/{mb['bw']:<6}")

    if out:
        print("-" * len(hdr))
        for cls in ("han", "latin", "mixed", "mono"):
            sub = [r for r in out if r["class"] == cls]
            if not sub:
                continue
            print(f"{cls:<6} n={len(sub):<3} "
                  f"墨量 {np.median([r['mass_r'] for r in sub]):.3f}  "
                  f"实心 {np.median([r['solid_r'] for r in sub]):.3f}  "
                  f"过渡 {np.median([r['trans_r'] for r in sub]):.3f}  "
                  f"面积 {np.median([r['area_r'] for r in sub]):.3f}  "
                  f"浓度 {np.median([r['dark_r'] for r in sub]):.3f}  "
                  f"彩边 {np.median([r['fringe_st'] for r in sub]):.1f}/"
                  f"{np.median([r['fringe_dw'] for r in sub]):.1f}  "
                  f"锐度 {np.median([r['sharp_st'] for r in sub]):.3f}/"
                  f"{np.median([r['sharp_dw'] for r in sub]):.3f}")
        allr = out
        print(f"全部   n={len(allr):<3} "
              f"墨量 {np.median([r['mass_r'] for r in allr]):.3f}  "
              f"实心 {np.median([r['solid_r'] for r in allr]):.3f}  "
              f"过渡 {np.median([r['trans_r'] for r in allr]):.3f}  "
              f"面积 {np.median([r['area_r'] for r in allr]):.3f}  "
              f"浓度 {np.median([r['dark_r'] for r in allr]):.3f}")
        dys = [r["dy"] for r in out]
        print(f"\n行带纵向偏移 dy(霜-DW)：中位 {np.median(dys):.0f}px  "
              f"范围 {min(dys)}~{max(dys)}px"
              f"（两侧基线口径差；量尺按各自带配对，已吸收）")

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=list(out[0].keys()))
            w.writeheader()
            w.writerows(out)
        print(f"\nCSV: {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
