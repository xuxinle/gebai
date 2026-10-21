#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""字重曲线调参工具：测量 → 报告 → （可选）自动微调。

`docs/WEIGHT_CURVE.md` 里的调参流程靠这个脚本闭环：

    测量各字号 → 看漂移 → 改 resources/text_weight_curve.txt → 再测量

为什么需要工具而不是手改手测：曲线有 9 个采样点、每点两个数值，
手工改完再逐个字号渲染对比，一轮就是十几分钟且容易漏档。

## 判据用「过渡带均值」，不用「平均覆盖率」

这是本项目踩过的坑：平均覆盖率会让大字号**天然偏高**——笔画宽、内部形成大片满黑
（9px 满黑占 0.8%，40px 占 47%），那是几何必然、正确的行为，不该被"纠正"。

「过渡带均值」= 只统计覆盖率落在 (0.05, 0.85) 的像素再求平均，
排除满黑区与抗锯齿尾巴，只反映**笔画边缘有多实**，才是"看着多重"的稳定代理。

## 用法

    python tools/dw_ab_sizecurve.py                 # 只测量并报告
    python tools/dw_ab_sizecurve.py --solve         # 额外给出建议的 gamma
    python tools/dw_ab_sizecurve.py --apply         # 直接把建议写回曲线文件
    python tools/dw_ab_sizecurve.py --theme light   # 亮色主题
    python tools/dw_ab_sizecurve.py --curve <path>  # 指定别的曲线文件

需要先构建探针（`build/probe/dw_ab_probe.exe`）。
"""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
D = ROOT / "build" / "dwab"
PROBE = ROOT / "build" / "probe" / "dw_ab_probe.exe"
DEFAULT_CURVE = ROOT / "resources" / "text_weight_curve.txt"

# 物理字号采样点（与曲线文件里的采样点一致最好，便于逐点对照）
SIZES = [9.0, 11.0, 13.0, 15.0, 18.0, 22.0, 26.0, 32.0, 40.0]

# 探针行表的基准字号是 15 物理 px（scale=1.0），所以 scale = 目标px / 15
BASE_PX = 15.0

# 过渡带：排除满黑(>=0.85)与几乎不可见(<=0.05)
EDGE_LO = 0.05
EDGE_HI = 0.85


def env_for(curve_path, theme):
    e = {k: v for k, v in os.environ.items() if k != "ST_TEXT_LCD"}
    if curve_path:
        e["ST_TEXT_WEIGHT_CURVE"] = str(curve_path)
    else:
        e.pop("ST_TEXT_WEIGHT_CURVE", None)
    return e


def measure(px, curve_path, theme, disable_curve=False):
    """渲染一个字号，返回 (过渡带均值, 平均覆盖率, 满黑占比)。"""
    if not PROBE.exists():
        raise SystemExit(f"探针不存在：{PROBE}\n先构建：st build --profile=dev")
    args = [str(PROBE), str(D), f"--scale={px / BASE_PX:.5f}",
            "--dark" if theme == "dark" else "--gray-off"]
    if theme != "dark":
        args = [str(PROBE), str(D), f"--scale={px / BASE_PX:.5f}"]
    if disable_curve:
        args.append("--no-curve")
    e = env_for(None if disable_curve else curve_path, theme)
    subprocess.run(args, check=True, capture_output=True, env=e)
    src = D / ("st-dark.png" if theme == "dark" else "st-light.png")
    tmp = D / "_sizecurve_tmp.png"
    if tmp.exists():
        tmp.unlink()
    shutil.move(str(src), str(tmp))
    stats = _analyze(tmp, theme)
    tmp.unlink()
    return stats


def _analyze(path, theme):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    bg = np.array([15.0, 17.0, 21.0]) if theme == "dark" else np.array([255.0, 255.0, 255.0])
    ink = np.abs(a - bg).max(2) > 20
    if not ink.any():
        return None
    lum = a @ np.array([0.299, 0.587, 0.114])
    lb = bg @ np.array([0.299, 0.587, 0.114])
    # 前景亮度用该图最亮的 0.5% 估计（字色是渲染器给定的确定值）
    lf = np.percentile(lum[ink], 99.5)
    if abs(lf - lb) < 1e-6:
        return None
    cov = np.clip((lum - lb) / (lf - lb), 0.0, 1.0)
    v = cov[cov > 0.05]
    if v.size < 50:
        return None
    edge = v[(v > EDGE_LO) & (v < EDGE_HI)]
    return dict(
        edge_mean=float(edge.mean()) if edge.size else 0.0,
        mean_cov=float(v.mean()),
        solid=100.0 * float((v > EDGE_HI).mean()),
    )


def read_curve(path):
    """读采样点：返回 [[px, gamma, contrast], ...]（按 px 升序）。"""
    rows = []
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8").split("\n"):
        body = line.split("#")[0].strip()
        if not body:
            continue
        parts = body.split()
        if len(parts) < 3:
            continue
        try:
            rows.append([float(parts[0]), float(parts[1]), float(parts[2])])
        except ValueError:
            continue
    rows.sort(key=lambda r: r[0])
    return rows


def write_curve(path, rows, notes=None):
    """把采样点写回文件，**保留文件头注释**。"""
    original = path.read_text(encoding="utf-8") if path.exists() else ""
    # 头部 = 第一个数据行之前的所有内容
    head_lines = []
    for line in original.split("\n"):
        body = line.split("#")[0].strip()
        is_data = False
        if body and len(body.split()) >= 3:
            try:
                float(body.split()[0])
                is_data = True
            except ValueError:
                is_data = False
        if is_data:
            break
        head_lines.append(line)
    while head_lines and head_lines[-1].strip() == "":
        head_lines.pop()
    lines = head_lines + ["", "# px     gamma  contrast  note"]
    for i, (px, g, c) in enumerate(rows):
        note = notes.get(round(px)) if notes else None
        suffix = f"  {note}" if note else ""
        lines.append(f"{px:>6.1f}   {g:>5.2f}  {c:>5.2f}{suffix}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--theme", choices=["dark", "light"], default="dark")
    ap.add_argument("--curve", default=str(DEFAULT_CURVE))
    ap.add_argument("--solve", action="store_true", help="给出建议 gamma")
    ap.add_argument("--apply", action="store_true", help="把建议写回曲线文件")
    ap.add_argument("--iterations", type=int, default=1)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    curve_path = pathlib.Path(a.curve)
    if not curve_path.is_absolute():
        curve_path = (ROOT / curve_path).resolve()

    for it in range(a.iterations):
        rows = read_curve(curve_path)
        if not rows:
            print(f"⚠ 曲线文件为空或不存在：{curve_path}")
            print("  （会以 --no-curve 基准跑一遍）")
        sizes = [r[0] for r in rows] if rows else SIZES

        print(f"\n{'=' * 66}")
        print(f"字重曲线测量  [{a.theme}]  {curve_path.name}"
              f"{f'  第 {it + 1}/{a.iterations} 轮' if a.iterations > 1 else ''}")
        print(f"{'=' * 66}")
        print(f"{'物理px':>8}{'过渡带均值':>12}{'平均覆盖率':>11}{'满黑%':>9}")

        measured = {}
        for px in sizes:
            st = measure(px, curve_path, a.theme)
            if st is None:
                print(f"{px:>8.1f}  (无墨像素，跳过)")
                continue
            measured[px] = st
            print(f"{px:>8.1f}{st['edge_mean']:>12.4f}{st['mean_cov']:>11.3f}{st['solid']:>9.1f}")
        if not measured:
            print("⚠ 没有任何有效测量")
            return 1

        edges = [v["edge_mean"] for v in measured.values()]
        spread = 100.0 * (max(edges) - min(edges)) / min(edges) if min(edges) > 0 else 0.0
        target = float(np.median(edges))
        print(f"\n过渡带漂移：{spread:.2f}%（目标值取中位 {target:.4f}）")
        verdict = ("跨字号已很一致" if spread < 6
                   else ("轻微漂移" if spread < 12 else "明显漂移，建议调参"))
        print(f"判定：{verdict}")

        if a.json:
            print(json.dumps({str(k): v for k, v in measured.items()},
                             ensure_ascii=False, indent=2))

        if not (a.solve or a.apply):
            continue

        # 响应斜率：每 +0.1γ 时过渡带均值的变化（实测值，按 px 插值）
        slope_px = [9.0, 13.0, 15.0, 22.0, 32.0, 40.0]
        slope_per_01 = [-0.0163, -0.0079, -0.0073, -0.0057, -0.0054, -0.0059]

        print(f"\n{'物理px':>8}{'当前γ':>8}{'偏差':>9}{'建议γ':>8}")
        print("-" * 36)
        by_px = {r[0]: r for r in rows}
        new_rows = []
        notes = {}
        for px, g, c in rows:
            if px not in measured:
                new_rows.append([px, g, c])
                continue
            cur = measured[px]["edge_mean"]
            need = target - cur
            slope = float(np.interp(px, slope_px, slope_per_01)) / 0.1  # 每 1.0γ 的变化
            dg = need / slope * 0.9 if abs(slope) > 1e-9 else 0.0   # 0.9 阻尼防振荡
            ng = max(0.30, min(4.0, g + dg))
            print(f"{px:>8.1f}{g:>8.2f}{need:>+9.4f}{ng:>8.2f}")
            new_rows.append([px, round(ng, 2), c])
            notes[round(px)] = None
        if a.apply:
            write_curve(curve_path, new_rows, notes)
            print(f"\n已写回 {curve_path}")
            print("建议再跑一次本脚本验证（不带 --apply）。")
        else:
            print("\n（--solve 只给建议；加 --apply 才会写回文件）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
