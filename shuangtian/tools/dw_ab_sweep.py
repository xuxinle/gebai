#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""霜天 × DWrite 参数扫描：量化"还有多少优化空间"。

回答两个问题：
1. **参照侧**：把 DWrite 的 ClearType 旋钮（增强对比度 / gamma）调一调，
   它能不能做到"墨量≈1、实心≈1、过渡≈1"三全？若不能，那 5% 的过渡差就不是缺陷。
2. **霜天侧**：亚像素滤波强度（ST_TEXT_LCD_TAPS）与主题 gamma 扫一遍，
   找出能让三量同时贴近参照的最优点，并看它离"完美"还有多远。

用法（在仓库根目录）：
  python tools/dw_ab_sweep.py --stage dw        # 只扫 DWrite 参照
  python tools/dw_ab_sweep.py --stage st        # 只扫霜天
  python tools/dw_ab_sweep.py --stage both
输出：build/dwab/sweep-report.txt（含矩阵与结论摘要）
"""
import argparse
import itertools
import json
import os
import pathlib
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
DWAB = ROOT / "build" / "dwab"
PROBE_ST = ROOT / "build" / "probe" / "dw_ab_probe.exe"
PROBE_DW = ROOT / "tools" / "dwrite_ref.exe"

BG = np.array([255.0, 255.0, 255.0])
FG = np.array([15.0, 23.0, 42.0])


def coverage(img):
    d = FG - BG
    denom = float((d * d).sum())
    t = ((img - BG) * d).sum(axis=2) / denom
    return np.clip(t, 0.0, 1.0)


def bands(t, min_ink=1.0, gap=2):
    prof = t.sum(axis=1)
    rows = np.where(prof > min_ink)[0]
    if rows.size == 0:
        return []
    out, start, prev = [], rows[0], rows[0]
    for y in rows[1:]:
        if y - prev > gap:
            out.append((int(start), int(prev) + 1))
            start = y
        prev = y
    out.append((int(start), int(prev) + 1))
    return out


def htrim(t, b):
    col = t[b[0]:b[1]].sum(axis=0)
    c = np.where(col > 0.5)[0]
    return None if c.size == 0 else (int(c[0]), int(c[-1]) + 1)


def row_metrics(t, y0, y1, x0, x1):
    sub = t[y0:y1, x0:x1]
    ink = sub > 0.15
    if not ink.any():
        return None
    n = int(ink.sum())
    return (float(sub.sum()), float((sub > 0.85).sum()) / n,
            float(((sub > 0.15) & (sub < 0.85)).sum()) / n, n)


def compare(st_img, ref_img, labels=None):
    """逐带配对比较，返回每条记录的 (mass_r, solid_r, trans_r)。"""
    t_st, t_rf = coverage(st_img), coverage(ref_img)
    bs, br = bands(t_st), bands(t_rf)
    out = []
    for i, (a, b) in enumerate(zip(bs, br)):
        ha, hb = htrim(t_st, a), htrim(t_rf, b)
        if ha is None or hb is None:
            continue
        ma = row_metrics(t_st, a[0], a[1], ha[0], ha[1])
        mb = row_metrics(t_rf, b[0], b[1], hb[0], hb[1])
        if ma is None or mb is None or mb[0] == 0 or mb[1] == 0 or mb[2] == 0:
            continue
        out.append((ma[0] / mb[0], ma[1] / mb[1], ma[2] / mb[2]))
    return out


def summarize(recs):
    if not recs:
        return (0, 0, 0, 9.9)
    arr = np.array(recs)
    med = np.median(arr, axis=0)
    # 目标：三量都=1。打分 = 到 (1,1,1) 的最大绝对偏差（保守：看最差的那一维）
    score = float(np.max(np.abs(med - 1.0)))
    return (med[0], med[1], med[2], score)


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", default="both", choices=["dw", "st", "both"])
    ap.add_argument("--ref", default="natural", help="DWrite 基准模式")
    args = ap.parse_args()
    lines = []

    def log(s=""):
        print(s)
        lines.append(s)

    # 基准参照：系统调校的自然渲染
    base_ref = DWAB / f"dw-{args.ref}-light.ppm"
    st_base = DWAB / "st-light.png"
    log("=" * 100)
    log(f"基线：st-light.png vs {base_ref.name}")
    recs = compare(load(st_base), load(base_ref))
    m = summarize(recs)
    log(f"  中位 → 墨量 {m[0]:.3f}  实心 {m[1]:.3f}  过渡 {m[2]:.3f}   最差维偏差 {m[3]:.3f}")
    log()

    # —— 阶段一：DWrite 参照自身扫旋钮，看"完美"能否达到 ——
    if args.stage in ("dw", "both"):
        log("=" * 100)
        log("DWrite 参照侧扫描（natural 模式，只调 ClearType 旋钮）")
        log(f"{'gamma':>7}{'contrast':>10}{'墨量':>8}{'实心':>8}{'过渡':>8}{'最差偏差':>10}")
        log("-" * 60)
        best = None
        for gamma, contrast in itertools.product([1.0, 1.8, 2.2], [0.0, 0.5, 1.0, 1.5]):
            tag = f"-g{gamma}-c{contrast}"
            out = DWAB / f"dwsweep-nat-light.ppm"
            # 用 tag 后缀写文件（避免覆盖基线）
            cmd = [str(PROBE_DW), str(DWAB / "rows-light.tsv"), str(DWAB),
                   "--theme", "light", "--mode", "natural",
                   "--gamma", str(gamma), "--contrast", str(contrast)]
            subprocess.run(cmd, check=True, capture_output=True)
            produced = DWAB / "dw-natural-light.ppm"
            variant = DWAB / f"dw-nat-g{gamma}-c{contrast}.ppm"
            produced.replace(variant)
            r = compare(load(st_base), load(variant))
            mm = summarize(r)
            log(f"{gamma:>7.2f}{contrast:>10.2f}{mm[0]:>8.3f}{mm[1]:>8.3f}{mm[2]:>8.3f}{mm[3]:>10.3f}")
            if best is None or mm[3] < best[0]:
                best = (mm[3], gamma, contrast, mm)
        log(f"→ DWrite 最优组合 gamma={best[1]} contrast={best[2]} 三量 "
            f"{best[3][0]:.3f}/{best[3][1]:.3f}/{best[3][2]:.3f}（最差偏差 {best[0]:.3f}）")
        # 恢复基线文件
        (DWAB / f"dw-nat-g{best[1]}-c{best[2]}.ppm").replace(DWAB / f"dw-{args.ref}-light.ppm")
        log()

    # —— 阶段二：霜天侧扫 taps × gamma ——
    if args.stage in ("st", "both"):
        log("=" * 100)
        log("霜天侧扫描（fit=off，类 gamma 固定；对基线 DWrite natural 参照）")
        log(f"{'taps':>28}{'gamma':>8}{'墨量':>8}{'实心':>8}{'过渡':>8}{'最差偏差':>10}")
        log("-" * 74)
        taps_list = [
            ("8,77,86,77,8", "默认(FreeType)"),
            ("0,74,92,74,0", "少旁瓣"),
            ("0,51,205,51,0", "锐(滤波最弱)"),
            ("16,68,88,68,16", "强滤波"),
        ]
        best_st = None
        for taps, name in taps_list:
            for gamma in [1.0, 1.10, 1.25, 1.40]:
                env = dict(os.environ)
                env["ST_TEXT_LCD_TAPS"] = taps
                out_png = DWAB / f"stsweep-{taps.replace(',', '_')}-g{gamma}.png"
                subprocess.run([str(PROBE_ST), str(DWAB), "--gamma=" + str(gamma)],
                               check=True, capture_output=True, env=env)
                (DWAB / "st-light.png").replace(out_png)
                r = compare(load(out_png), load(base_ref))
                mm = summarize(r)
                log(f"{taps + ' ' + name:>28}{gamma:>8.2f}{mm[0]:>8.3f}{mm[1]:>8.3f}"
                    f"{mm[2]:>8.3f}{mm[3]:>10.3f}")
                if best_st is None or mm[3] < best_st[0]:
                    best_st = (mm[3], taps, gamma, mm, out_png)
        log(f"→ 霜天最优 taps={best_st[1]} gamma={best_st[2]} 三量 "
            f"{best_st[3][0]:.3f}/{best_st[3][1]:.3f}/{best_st[3][2]:.3f}"
            f"（最差偏差 {best_st[0]:.3f}）")
        # 恢复默认基线图
        env = dict(os.environ)
        env.pop("ST_TEXT_LCD_TAPS", None)
        subprocess.run([str(PROBE_ST), str(DWAB)], check=True, capture_output=True, env=env)
        log()

    (DWAB / "sweep-report.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"\n报告写入 {DWAB / 'sweep-report.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
