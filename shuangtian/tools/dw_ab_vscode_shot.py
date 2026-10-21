#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""实测 VS Code 的真实字体渲染，判定它用的是哪种方案（灰度 / ClearType / 对比度多少）。

为什么必须实测：网上说法互相矛盾（"VS Code 用 ClearType" vs "Chromium 早已改灰度"），
而不同版本/系统设置下确实不同。**参照选错，后面所有像素级复刻都是错的。**

做法：
1. 生成一个专用文件（固定字体/字号/颜色，与 theme token 一致）；
2. 用 VS Code 打开并全屏，截图；
3. 按 VS Code Dark+ 的底/字色算覆盖率，与 DWrite 各候选参数比对 RMS。

用法：python tools/dw_ab_vscode_shot.py [--capture] [--compare]
  --capture  启动 VS Code 截图（需要前台，可能被遮挡）
  --compare  与已有截图比对
"""
import argparse
import json
import pathlib
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
D = ROOT / "build" / "dwab"
VSCODE = r"C:\Users\Administrator\AppData\Local\Programs\Microsoft VS Code\Code.exe"

# VS Code Dark+ 的实际取值（实测截图后校正）
DARK_BG = np.array([30.0, 30.0, 30.0])     # #1E1E1E
DARK_FG = np.array([212.0, 212.0, 212.0])  # #D4D4D4


def make_sample():
    """生成用于比对的样本文件（与 DWrite 探针行表同文本）。"""
    path = D / "vscode-sample.tsx"
    lines = [
        "霜天渲染 中文 Abc 123",
        "疆馨密霸繁悬整靓霾",
        "一二十上下三人",
        "Hamburgefonstiv III",
        "illumination minimal",
        "0123456789 3.14 42",
        "->=>(){}[]; if (x) {",
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def capture(wait=8.0):
    sample = make_sample()
    # 固定设置：关闭连字、固定字体/字号，保证可比
    args = [
        VSCODE,
        "--new-window",
        "--disable-extensions",
        "--user-data-dir", str(ROOT / ".st" / "vscode-profile"),
        "--locale", "en",
        str(sample),
    ]
    print("启动 VS Code：", " ".join(args))
    proc = subprocess.Popen(args)
    try:
        import time
        time.sleep(wait)
        out = D / "vscode-shot.png"
        ps = f"""
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing,System.Windows.Forms
Add-Type @'
using System;using System.Runtime.InteropServices;
public class W {{
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int w,int t,uint f);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [StructLayout(LayoutKind.Sequential)] public struct RECT {{ public int L,T,R,B; }}
}}
'@
$p = Get-Process | Where-Object {{ $_.ProcessName -match 'Code' -and $_.MainWindowHandle -ne 0 }} | Select-Object -First 1
if (-not $p) {{ Write-Output 'no-window'; exit 1 }}
$h = $p.MainWindowHandle
[W]::ShowWindow($h, 3) | Out-Null
[W]::SetWindowPos($h, [IntPtr](-1), 0, 0, 900, 700, 0x0040) | Out-Null
[W]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 2500
$r = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $t = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $t
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $t))
$bmp.Save('{out}', [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "shot-ok ${{w}}x${{t}}"
"""
        r = subprocess.run(["pwsh", "-NoProfile", "-Command", ps],
                           capture_output=True, text=True)
        print(r.stdout.strip() or r.stderr.strip())
        return out if out.exists() else None
    finally:
        proc.terminate()


def coverage(img, bg, fg):
    dd = fg - bg
    return np.clip(((img - bg) * dd).sum(2) / float((dd * dd).sum()), 0.0, 1.0)


def analyze(shot: pathlib.Path):
    """从 VS Code 截图里找出代码文字区，量它的覆盖率分布。"""
    img = np.asarray(Image.open(shot).convert("RGB"), dtype=np.float64)
    print(f"截图 {img.shape[1]}x{img.shape[0]}")
    # 找 VS Code 编辑区：大面积 #1E1E1E
    bgmask = (np.abs(img - DARK_BG).max(2) < 8)
    print(f"  #1E1E1E 占比 {100.0*bgmask.mean():.1f}%")
    if bgmask.mean() < 0.1:
        print("  ⚠ 没找到编辑器主体（可能被遮挡或主题不同）")
    # 覆盖率
    t = coverage(img, DARK_BG, DARK_FG)
    ink = t > 0.15
    if not ink.any():
        print("  ⚠ 无墨像素")
        return None
    n = int(ink.sum())
    stats = {
        "ink": n,
        "solid_pct": 100.0 * (t > 0.85).sum() / n,
        "trans_pct": 100.0 * ((t > 0.15) & (t < 0.85)).sum() / n,
        "full_pct": 100.0 * (t > 0.99).sum() / n,
        "mean_cov": float(t[ink].mean()),
    }
    # 彩边：VS Code 若用 ClearType，会有明显通道差
    ch = (img.max(2) - img.min(2))[ink]
    stats["chroma_mean"] = float(ch.mean())
    stats["chroma_p99"] = float(np.percentile(ch, 99))
    print(f"  墨像素 {n}  实心 {stats['solid_pct']:.1f}%  过渡 {stats['trans_pct']:.1f}%"
          f"  平均覆盖 {stats['mean_cov']:.3f}")
    print(f"  彩边均值 {stats['chroma_mean']:.1f}  P99 {stats['chroma_p99']:.0f}"
          f"  → {'灰度（无彩边）' if stats['chroma_mean'] < 8 else '亚像素 ClearType（有彩边）'}")
    return stats


def compare_with_dw(shot: pathlib.Path):
    """把 VS Code 的文字区与 DWrite 各候选参数逐像素比 RMS。"""
    vstats = analyze(shot)
    if vstats is None:
        return
    print("\n与 DWrite 各候选参数比对（只比分布，不比位置）")
    print(f"{'候选':<20}{'实心%':>8}{'过渡%':>8}{'平均覆盖':>10}{'彩边':>8}{'分布距离':>10}")
    print("-" * 66)
    best = None
    for p in sorted(D.glob("dwv-*.ppm")):
        img = np.asarray(Image.open(p).convert("RGB"), dtype=np.float64)
        t = coverage(img, np.array([14.0, 17.0, 21.0]), np.array([212.0, 212.0, 216.0]))
        ink = t > 0.15
        if not ink.any():
            continue
        n = int(ink.sum())
        solid = 100.0 * (t > 0.85).sum() / n
        trans = 100.0 * ((t > 0.15) & (t < 0.85)).sum() / n
        mean = float(t[ink].mean())
        ch = float((img.max(2) - img.min(2))[ink].mean())
        dist = (abs(solid - vstats["solid_pct"]) + abs(trans - vstats["trans_pct"])
                + abs(mean - vstats["mean_cov"]) * 100)
        print(f"{p.stem:<20}{solid:>8.1f}{trans:>8.1f}{mean:>10.3f}{ch:>8.1f}{dist:>10.2f}")
        if best is None or dist < best[0]:
            best = (dist, p.stem)
    if best:
        print(f"\n→ 最接近 VS Code 的参数：{best[1]}（距离 {best[0]:.2f}）")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", action="store_true")
    ap.add_argument("--compare", action="store_true")
    a = ap.parse_args()
    shot = D / "vscode-shot.png"
    if a.capture:
        shot = capture() or shot
    if a.compare or not a.capture:
        if not shot.exists():
            print(f"缺截图 {shot}（先跑 --capture）", file=sys.stderr)
            return 1
        compare_with_dw(shot)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
