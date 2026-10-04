#!/usr/bin/env python3
"""深色主题的真窗口基准：行投影分带 + 逐带比**整幅总墨量**。

与 `realwin_ink2.py` 同一套"不裁剪、不对齐"的口径，差别只有前景/背景色对调：
  · 浅色：BG=`#FFFFFF`，FG=`#0F172A`；覆盖率 = 像素投影到 [BG,FG] 连线
  · 深色：BG=`#0A0F1A`，FG=`#E8EEF9`（主题 token 原值）
反色不是同一个式子——深色下"字更粗"表现为**亮度更高**，所以前景色必须换成浅色，
否则积分出来是负值。

**为什么深色要单独标**：预校正公式 `α' = 1−(1−α)^(1/γ)` 的方向是按黑字白底推导的
（γ<1 压黑、γ>1 提亮），白字黑底的观感由反方向对比决定，浅色标出的 γ 不能直接沿用。

用法：echo "<字号列表>" | python tools/realwin_ink_dark.py <基准png> <霜天目录> <模板> <gamma...>
"""

import sys

import numpy as np
from PIL import Image

# 深色主题 token（src/ui/theme.cpp Theme::dark）
BG = np.array([0x0A, 0x0F, 0x1A], dtype=float)
FG = np.array([0xE8, 0xEE, 0xF9], dtype=float)
INK_TH = 0.02


def cov(arr):
    d = FG - BG
    return np.clip(((arr - BG) @ d) / float(d @ d), 0.0, 1.0)


def page_columns(img):
    """页面（深底）的列范围：取**最长连续"接近主题底色"**的列区。

    深底页上"页面"比窗口外更暗，取法同浅色（找最长连续一致区），只是判据换成
    与主题底色的距离，而不是"够白"。
    """
    a = np.asarray(img.convert("RGB")).astype(float)
    med = np.median(a, axis=0)
    near = np.abs(med - BG).max(axis=1) < 24.0
    best, start = (0, 0, 0), None
    for x, f in enumerate(near):
        if f and start is None:
            start = x
        elif not f and start is not None:
            if x - start > best[0]:
                best = (x - start, start, x)
            start = None
    if start is not None and len(near) - start > best[0]:
        best = (len(near) - start, start, len(near))
    return best[1] + 6, best[2] - 6


def find_content_top(img, x0, x1):
    """找**页面内容区**的起始行。

    ⚠ **不能靠"接近底色"**：深色主题下浏览器工具栏与页面都是深色，两者分不开
    （实测：工具栏被并成一条 142px 的假带）。靠**框的边框色**才可靠——
    对照页用青色 `#00E5FF` 描边，它在深底/浅底上都与背景相差极大。
    """
    a = np.asarray(img.convert("RGB")).astype(int)
    # 青色边：G/B 高、R 低
    edge = (a[:, :, 0] < 120) & (a[:, :, 1] > 170) & (a[:, :, 2] > 190)
    rows = np.where(edge.sum(axis=1) > 200)[0]
    return int(rows[0]) if rows.size else 0


def bands(img, x0, x1, th=INK_TH, gap=8):
    a = np.asarray(img.convert("RGB")).astype(float)[:, x0:x1, :]
    prof = cov(a).sum(axis=1) / max(1, (x1 - x0))
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
    return [(a0, a1) for a0, a1 in out if a1 - a0 >= 5]


def main() -> int:
    ref_path, st_dir, tmpl = sys.argv[1], sys.argv[2], sys.argv[3]
    gammas = sys.argv[4:] or ["100", "110", "120"]
    sizes = [int(x) for x in sys.stdin.read().lstrip("\ufeff").split()]
    ref = Image.open(ref_path)
    x0, x1 = page_columns(ref)
    top = find_content_top(ref, x0, x1)
    if top > 0:
        ref = ref.crop((0, top, ref.size[0], ref.size[1]))
    print(f"页面列范围 {x0}..{x1}；内容起始行 {top}（已裁）")
    bs = bands(ref, x0, x1)
    print(f"行投影检出 {len(bs)} 条带（期望 {len(sizes)}）: {[b[1]-b[0] for b in bs]}")
    if len(bs) > len(sizes):
        print("带数多于字号数——先确认页面布局")
        return 1
    if len(bs) < len(sizes):
        print(f"一屏只显示 {len(bs)}/{len(sizes)} 框，本次只测前 {len(bs)} 个字号")
        sizes = sizes[:len(bs)]
    print(f"{'逻辑px':>7}  " + "  ".join(f"γ{g:>4}" for g in gammas) + "   理想γ")
    print("-" * 72)
    for size, (y0, y1) in zip(sizes, bs):
        cr = cov(np.asarray(ref.convert("RGB")).astype(float)[y0:y1, x0:x1, :])
        rx = np.where((cr > 0.12).any(axis=0))[0]
        ref_ink = float(cr[:, rx[0]:rx[-1] + 1].sum()) if rx.size else 0.0
        ratios = []
        for g in gammas:
            st = Image.open(f"{st_dir}/{tmpl.format(g=g, s=size)}")
            cs = cov(np.asarray(st.convert("RGB")).astype(float))
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
