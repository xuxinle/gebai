#!/usr/bin/env python3
"""真窗口浏览器基准：**行投影**找文字带，逐带比整幅总墨量。

## 为什么不按红框/坐标切

真窗口截图含工具栏与滚动条，任何"窗口几何 + 页面坐标"的假设都不成立（实测踩到：
滚动位置让框与页面坐标错开 200+ px）。红描边也不稳（部分边框被抗锯齿削到阈值以下，
只检出 8 条里的 5 条）——**能自动认出来的只有"哪几行有字"**。

本脚本只依赖一件事：**每框一行字、框间距远大于行高**，所以行投影能干净分带。
每带单独积分，与霜天单行图（整幅积分）相比。

用法：echo "<字号列表>" | python tools/realwin_ink2.py <基准png> <霜天目录> <模板> <gamma...>
"""

import sys

import numpy as np
from PIL import Image

FG = np.array([0x0F, 0x17, 0x2A], dtype=float)
BG = np.array([255.0, 255.0, 255.0])
# 行投影阈值：整幅宽度摊薄后，单行文字的暗度均值量级见下方注释
INK_TH = 0.02


def cov(arr):
    d = BG - FG
    return np.clip(((BG - arr) @ d) / float(d @ d), 0.0, 1.0)


def bands(img, x0, x1, th=INK_TH, gap=8):
    a = np.asarray(img.convert("RGB")).astype(float)[:, x0:x1, :]
    c = cov(a)
    prof = c.sum(axis=1) / max(1, (x1 - x0))
    on = prof > th
    out, start, blanks = [], None, 0
    for y, f in enumerate(on):
        if f:
            if start is None:
                start = y
            blanks = 0
        elif start is not None:
            blanks += 1
            if blanks > gap:
                out.append((start, y - blanks + 1))
                start = None
                blanks = 0
    if start is not None:
        out.append((start, len(on)))
    # 丢掉过矮的带（噪声）
    return [(a0, a1) for a0, a1 in out if a1 - a0 >= 5]


def page_columns(img):
    """找**页面白区**的列范围。

    真窗口截图里窗口外会有别的窗口露出（实测踩到：左侧一条暗色带伸进了
    `[2%, 86%]` 全图范围，把行投影整体抬高、所有带被并成一条）。
    所以不能按“全图比例”取列——必须找到**真正属于页面的白区**。
    """
    a = np.asarray(img.convert("L")).astype(float)
    med = np.median(a, axis=0)
    white = med > 200.0
    # 取最长连续白区
    best, start = (0, 0, 0), None
    for x, f in enumerate(white):
        if f and start is None:
            start = x
        elif not f and start is not None:
            if x - start > best[0]:
                best = (x - start, start, x)
            start = None
    if start is not None and len(white) - start > best[0]:
        best = (len(white) - start, start, len(white))
    return best[1] + 6, best[2] - 6


def find_content_top(img, x0, x1):
    """找**页面内容区**的起始行（跳过浏览器工具栏/页签条）。

    不能从 y=0 开始投影：工具栏区域在 x0..x1 上会是暗的，投影出一条横跨全宽的
    巨大假带（实测：检出 142px 的假带）。做法是找“连续 8 行近乎全白”的第一处。
    """
    a = np.asarray(img.convert("L")).astype(float)[:, x0:x1]
    med = np.median(a, axis=1)
    run = 0
    for y, v in enumerate(med):
        if v > 200.0:
            run += 1
            if run >= 8:
                return y - 7
        else:
            run = 0
    return 0


def main() -> int:
    ref_path, st_dir, tmpl = sys.argv[1], sys.argv[2], sys.argv[3]
    gammas = sys.argv[4:] or ["060", "075", "090", "105"]
    sizes = [int(x) for x in sys.stdin.read().lstrip("\ufeff").split()]
    ref = Image.open(ref_path)
    x0, x1 = page_columns(ref)
    top = find_content_top(ref, x0, x1)
    if top > 0:
        ref = ref.crop((0, top, ref.size[0], ref.size[1]))
    print(f"页面白区列范围 {x0}..{x1}；内容起始行 {top}（已从截图裁掉）")
    bs = bands(ref, x0, x1)
    print(f"行投影检出 {len(bs)} 条文字带（期望 {len(sizes)}）: {[b[1]-b[0] for b in bs]}")
    if len(bs) > len(sizes):
        print(f"检出带数 {len(bs)} 多于字号数 {len(sizes)}——先确认页面布局")
        return 1
    if len(bs) < len(sizes):
        # 一屏放不下全部框时，只测**可见的前 N 框**（顺序一致，前 N 个就是前 N 个字号）。
        # 这是设计允许的：真窗口截图受窗口高度限制，分批测即可。
        print(f"一屏只显示 {len(bs)}/{len(sizes)} 框，本次只测前 {len(bs)} 个字号")
        sizes = sizes[:len(bs)]
    print(f"{'逻辑px':>7}  " + "  ".join(f"γ{g:>4}" for g in gammas) + "   理想γ")
    print("-" * 72)
    for size, (y0, y1) in zip(sizes, bs):
        ra = np.asarray(ref.convert("RGB")).astype(float)[y0:y1, x0:x1, :]
        # 水平也收到墨迹范围，去掉两侧留白（不影响比值，但去掉更稳）
        cr = cov(ra)
        rx = np.where((cr > 0.12).any(axis=0))[0]
        ref_ink = float(cr[:, rx[0]:rx[-1] + 1].sum()) if rx.size else 0.0
        ratios = []
        for g in gammas:
            st = Image.open(f"{st_dir}/{tmpl.format(g=g, s=size)}")
            sa = np.asarray(st.convert("RGB")).astype(float)
            cs = cov(sa)
            sx = np.where((cs > 0.12).any(axis=0))[0]
            st_ink = float(cs[:, sx[0]:sx[-1] + 1].sum()) if sx.size else 0.0
            ratios.append(st_ink / ref_ink if ref_ink > 0 else float("nan"))
        gv = [float(g[:1] + "." + g[1:]) for g in gammas]
        ideal = None
        for k in range(len(ratios) - 1):
            if (ratios[k] - 1.0) * (ratios[k + 1] - 1.0) <= 0.0 and ratios[k] != ratios[k + 1]:
                t = (1.0 - ratios[k]) / (ratios[k + 1] - ratios[k])
                ideal = gv[k] + t * (gv[k + 1] - gv[k])
                break
        print(f"{size:>7}  " + "  ".join(f"{r:>6.3f}" for r in ratios)
              + f"   {('%.3f' % ideal) if ideal is not None else '  —  '}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
