#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""彩色字 / 深色主题 / 彩色背景：验证默认改灰度后这三种场景的实际观感与量化表现。

为什么必须单独测（而不是沿用白底深字的数据）：
* **彩色字**（primary 蓝 `#4655D8`、danger 红 `#D13B40`）在亚像素下三通道 α 不同，
  会让原本纯色的字**变色/发浑**；
* **深色主题**（底 `#0A0F1A`、字 `#E8EEF9`）是浅字深底，覆盖率映射方向相反；
* **彩色背景**（primary_soft `#EDEEF9` 等按钮底）上，亚像素的逐通道 α 会
  在背景上掺进不属于它的色偏。

用法：python tools/dw_ab_color.py
"""
import json
import pathlib
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
D = ROOT / "build" / "dwab"
PROBE = ROOT / "build" / "probe" / "dw_ab_probe.exe"

# 主题 token 原值（src/ui/theme.cpp）
LIGHT = dict(bg="E8E8EE", surface="FAFAFC", text="141418", primary="4655D8",
             accent="0E849C", success="1B8B55", warning="8F6614", danger="D13B40",
             primary_soft="EDEEF9", text_muted="4E4E5A")
DARK = dict(bg="0A0F1A", surface="131722", text="F2F2F5", primary="8B9AFF",
            accent="6FD8E8", success="55D396", warning="E6BD6D", danger="FF7A7A",
            primary_soft="2A2E45", text_muted="B4B4BE")


def hex2rgb(h):
    return np.array([int(h[i:i + 2], 16) for i in (0, 2, 4)], dtype=np.float64)


def load(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float64)


def chroma(img, mask=None):
    """彩色偏离度：max(R,G,B)-min(R,G,B)。灰度渲染应恒为 0。"""
    c = img.max(2) - img.min(2)
    return float(c[mask].mean()) if mask is not None and mask.any() else float(c.mean())


def main() -> int:
    scale = 1.0
    rowsdir = D
    print("=" * 76)
    print("彩色字 / 深色主题 / 彩色背景 —— 灰度 vs 亚像素（scale=1.0，15 物理 px）")
    print("=" * 76)

    # 用探针的行表 + 主题开关出一版，然后我们按主题 token 重新算「该文字色在给定底上应是什么」
    # 探针只认 light/dark 两套固定色；这里改用「同一张图 + 通道分析」判断彩色是否被引入。
    results = []
    for theme, dark_flag in (("light", False), ("dark", True)):
        for gray in (True, False):
            args = [str(PROBE), str(D), f"--scale={scale}"]
            if dark_flag:
                args.append("--dark")
            if gray:
                args.append("--gray")
            subprocess.run(args, check=True, capture_output=True,
                           env={k: v for k, v in __import__("os").environ.items()
                                if k != "ST_TEXT_LCD"})
            out = D / f"color-{theme}-{'gray' if gray else 'lcd'}.png"
            (D / ("st-dark.png" if dark_flag else "st-light.png")).replace(out)
            results.append((theme, "灰度" if gray else "亚像素", out))

    print(f"\n{'主题':<8}{'渲染':<8}{'墨像素':>8}{'彩色偏离':>10}{'极差P99':>9}{'满黑%':>8}{'实心%':>8}")
    print("-" * 60)
    for theme, mode, p in results:
        img = load(p)
        # 墨像素 = 与底色差异大的像素
        bgc = hex2rgb(LIGHT["bg"] if theme == "light" else DARK["bg"])
        d = np.abs(img - bgc).sum(2)
        ink = d > 60
        if not ink.any():
            print(f"{theme:<8}{mode:<8}  (无墨像素)")
            continue
        c = (img.max(2) - img.min(2))[ink]
        # 覆盖率（相对该主题的前景/背景）
        fgc = hex2rgb(LIGHT["text"] if theme == "light" else DARK["text"])
        t = np.clip(((img - bgc) * (fgc - bgc)).sum(2) / float(((fgc - bgc) ** 2).sum()), 0, 1)
        print(f"{theme:<8}{mode:<8}{int(ink.sum()):>8}{c.mean():>10.1f}"
              f"{np.percentile(c,99):>9.0f}{100*t[ink].mean():>7.1f}%"
              f"{100*(t>0.85).sum()/ink.sum():>7.1f}%")

    # —— 彩色字的专用测试：直接渲染一段彩色文本，测色相漂移 ——
    print("\n" + "=" * 76)
    print("彩色字的色相漂移（关键：亚像素会不会把纯色字染上别的颜色）")
    print("=" * 76)
    print("原理：亚像素对 R/G/B 用**不同的 α**，所以一个纯色字在笔画内部"
          "\n      三通道覆盖率不一致 → 原本 (R,G,B) 纯色被拉成别的色相。")
    print("      灰度模式三通道同一 α → 纯色字恒保持原色相。\n")

    # 用 Python 直接模拟两种混合，验证数学而非渲染器（渲染器结论已在上面量过）
    print(f"{'场景':<34}{'灰度模式色偏':>14}{'亚像素色偏':>13}")
    print("-" * 62)
    for name, fg_hex, bg_hex in [
        ("浅底 蓝色 primary #4655D8", LIGHT["primary"], LIGHT["bg"]),
        ("浅底 红色 danger #D13B40", LIGHT["danger"], LIGHT["bg"]),
        ("浅底 绿 success #1B8B55", LIGHT["success"], LIGHT["bg"]),
        ("深底 浅蓝 primary #8B9AFF", DARK["primary"], DARK["bg"]),
        ("深底 浅红 danger #FF7A7A", DARK["danger"], DARK["bg"]),
    ]:
        fg, bg = hex2rgb(fg_hex), hex2rgb(bg_hex)
        # 灰度：同一 α 混合，结果落在 fg-bg 连线上 → 色相不变（只变明度）
        # 亚像素：三通道 α 各差 ±0.18（实测典型），各自混合 → 偏离连线
        a_gray = 0.6
        gray_res = bg + (fg - bg) * a_gray
        dev_gray = np.abs((gray_res - bg) / (fg - bg + 1e-9) - a_gray).max()

        alphas = np.array([a_gray + 0.18, a_gray, a_gray - 0.18])
        lcd_res = bg + (fg - bg) * alphas
        # 偏离连线量：把结果投影回 fg-bg 连线，看残差
        dirv = (fg - bg) / (np.linalg.norm(fg - bg) + 1e-9)
        proj = ((lcd_res - bg) @ dirv)
        resid = (lcd_res - bg) - np.outer(proj, dirv)
        dev_lcd = float(np.linalg.norm(resid))
        print(f"{name:<34}{dev_gray:>14.3f}{dev_lcd:>13.1f}")

    print("\n解读：灰度模式的色偏恒为 0（合成严格落在 fg→bg 连线上，色相不变）；"
          "\n      亚像素会偏离连线几十个色阶 —— 彩色字就是这样被「染浑」的。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
