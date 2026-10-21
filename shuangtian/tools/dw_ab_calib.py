#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按 DWrite 参照标定霜天的 LCD 滤波与覆盖率校正（类 γ 修复后重做）。

背景：此前标定跑在「类 γ 门禁 bug」下（全局 γ=1.0 时类 γ 整段被跳过），
所以 `γ=1.0 + 类γ` 与 `γ=1.0` 结果完全相同是假象。现在门禁已修，
必须重新扫一遍，否则调出来的默认值是错的。

标定目标 = **DWrite 直调**（用户指定"按 DWrite 调"）。
三个量同时贴近才叫对：实心比、过渡比、每墨像素墨量。

用法：python tools/dw_ab_calib.py [--scale 1.5] [--ref natural|gdi]
"""
import argparse
import itertools
import json
import os
import pathlib
import subprocess

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
D = ROOT / "build" / "dwab"
PROBE = ROOT / "build" / "probe" / "dw_ab_probe.exe"
DWREF = ROOT / "tools" / "dwrite_ref.exe"

BG = np.array([255., 255., 255.])
FG = np.array([15., 23., 42.])


def cov(p):
    a = np.asarray(Image.open(p).convert("RGB"), dtype=np.float64)
    d = FG - BG
    return np.clip(((a - BG) * d).sum(2) / float((d * d).sum()), 0, 1)


def bands(t, mi=1.0, gap=2):
    prof = t.sum(1)
    rows = np.where(prof > mi)[0]
    if rows.size == 0:
        return []
    out, s, p = [], rows[0], rows[0]
    for y in rows[1:]:
        if y - p > gap:
            out.append((int(s), int(p) + 1))
            s = y
        p = y
    out.append((int(s), int(p) + 1))
    return out


def htrim(t, b):
    c = np.where(t[b[0]:b[1]].sum(0) > 0.5)[0]
    return (int(c[0]), int(c[-1]) + 1) if c.size else None


def per_line(tst, trf, n_expect):
    bs, br = bands(tst), bands(trf)
    if len(bs) != len(br):
        return None
    recs = []
    for b1, b2 in zip(bs, br):
        h1, h2 = htrim(tst, b1), htrim(trf, b2)
        if not h1 or not h2:
            continue
        a = tst[b1[0]:b1[1], h1[0]:h1[1]]
        b = trf[b2[0]:b2[1], h2[0]:h2[1]]
        ia, ib = a > 0.15, b > 0.15
        if not ia.any() or not ib.any():
            continue
        sa = (a > 0.85).sum() / ia.sum()
        sb = (b > 0.85).sum() / ib.sum()
        ta = ((a > 0.15) & (a < 0.85)).sum() / ia.sum()
        tb = ((b > 0.15) & (b < 0.85)).sum() / ib.sum()
        recs.append(dict(mass=a.sum() / b.sum(),
                         solid=sa / sb if sb else 0,
                         trans=ta / tb if tb else 0,
                         ink=ia.sum() / ib.sum(),
                         dark=a[ia].mean() / b[ib].mean()))
    return recs


def med(recs, key):
    return float(np.median([r[key] for r in recs])) if recs else 0.0


def score(recs):
    """三量等权：越接近 1.0 越好。"""
    if not recs:
        return 9.9
    return (abs(med(recs, "solid") - 1) + abs(med(recs, "trans") - 1)
            + abs(med(recs, "mass") - 1))


def run_probe(scale, extra, env_extra):
    env = {k: v for k, v in os.environ.items() if k != "ST_TEXT_LCD_TAPS"}
    env.update(env_extra)
    subprocess.run([str(PROBE), str(D), f"--scale={scale}"] + extra,
                   check=True, capture_output=True, env=env)
    out = D / "calib-tmp.png"
    if out.exists():
        out.unlink()
    (D / "st-light.png").rename(out)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scale", type=float, default=1.5)
    ap.add_argument("--ref", default="natural")
    ap.add_argument("--no-dw", action="store_true", help="跳过重生成 DWrite 参照")
    a = ap.parse_args()

    # 参照（几何必须与霜天同尺寸 → 用同一 scale 重生成）
    meta = json.loads((D / "rows-light.json").read_text(encoding="utf-8"))
    lw, lh = meta["width"], meta["height"]
    ref_path = D / f"dw-{a.ref}-light.ppm"
    if not a.no_dw:
        subprocess.run([str(DWREF), str(D / "rows-light.tsv"), str(D), "--theme", "light",
                        "--mode", a.ref, "--scale", str(a.scale), "--h", str(lh)],
                       check=True, capture_output=True)
    trf = cov(ref_path)
    n_expect = len(bands(trf))

    print(f"scale={a.scale}  参照={a.ref}  DWrite 带数={n_expect}")
    print(f"{'taps':<20}{'γ':>6}{'类γ':>5}{'墨量':>8}{'实心':>8}{'过渡':>8}{'墨像素':>8}{'评分':>7}")
    print("-" * 76)

    taps_list = ["8,77,86,77,8", "5,74,98,74,5", "0,74,92,74,0", "0,51,205,51,0", "0,0,256,0,0"]
    results = []
    for taps, gamma, cls in itertools.product(taps_list, [1.00, 1.10], [True, False]):
        f = run_probe(a.scale, [f"--gamma={gamma:.2f}"] + ([] if cls else ["--no-class-gamma"]),
                      {"ST_TEXT_LCD_TAPS": taps})
        recs = per_line(cov(f), trf, n_expect)
        if recs is None:
            print(f"{taps:<20}{gamma:>6.2f}{'开' if cls else '关':>5}   带数不符，跳过")
            f.unlink()
            continue
        s = score(recs)
        results.append((s, taps, gamma, cls, recs))
        print(f"{taps:<20}{gamma:>6.2f}{'开' if cls else '关':>5}{med(recs,'mass'):>8.3f}"
              f"{med(recs,'solid'):>8.3f}{med(recs,'trans'):>8.3f}{med(recs,'ink'):>8.3f}{s:>7.3f}")
        f.unlink()

    results.sort(key=lambda r: r[0])
    print("\n" + "=" * 76)
    print("最优 3 组：")
    for s, taps, gamma, cls, recs in results[:3]:
        print(f"  taps={taps:<18} γ={gamma:.2f} 类γ={'开' if cls else '关'}  "
              f"墨量 {med(recs,'mass'):.3f}  实心 {med(recs,'solid'):.3f}  "
              f"过渡 {med(recs,'trans'):.3f}  评分 {s:.3f}")

    s, taps, gamma, cls, recs = results[0]
    print(f"\n推荐：ST_TEXT_LCD_TAPS={taps}  γ={gamma:.2f}  类γ={'开' if cls else '关'}")
    by_cls = {}
    for r in recs:
        by_cls.setdefault("all", []).append(r)
    print(f"  评分 {s:.3f}（0 = 与 DWrite 完全一致）")
    (D / "calib-report.json").write_text(json.dumps(
        {"scale": a.scale, "ref": a.ref, "best": {"taps": taps, "gamma": gamma,
                                                  "class_gamma": cls, "score": s,
                                                  "median": {"mass": med(recs, 'mass'),
                                                             "solid": med(recs, 'solid'),
                                                             "trans": med(recs, 'trans')}},
         "all": [{"taps": t, "gamma": g, "class": c, "score": sc} for sc, t, g, c, _ in results]},
        ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"\n报告：{D / 'calib-report.json'}")


if __name__ == "__main__":
    main()
