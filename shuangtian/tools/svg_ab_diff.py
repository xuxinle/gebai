#!/usr/bin/env python3
"""SVG 矢量图形 A/B：逐 case 比霜天与浏览器的渲染差异。

与 `text_ab_diff.py` 共用覆盖率/统计基建；每个 case 独立成带（generator 的布局是
`14px 标题 + 64px 画布`），所以差异能归因到**具体某个 SVG 特性**。
"""
import importlib.util
import json
import pathlib
import sys

import numpy as np
from PIL import Image

TOOLS = pathlib.Path('/workspace/gebai/shuangtian/tools')
spec = importlib.util.spec_from_file_location("ab", TOOLS / "text_ab_diff.py")
ab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ab)

dark = '--dark' in sys.argv
sfx = '_dark' if dark else ''
bg = np.array([0x0F, 0x11, 0x15] if dark else [255, 255, 255], dtype=float)
ab.BG = bg

d = pathlib.Path('/tmp/ab-svg2')
st = ab.load(d / f"svg-ab{'-dark' if dark else ''}-st.png")
br = ab.load(d / f"svg-ab-br{'' if not dark else '_dark'}.png")
h = min(st.shape[0], br.shape[0]); w = min(st.shape[1], br.shape[1])
st, br = st[:h, :w], br[:h, :w]

meta = json.loads((TOOLS / f"svg_ab_cases{sfx}.json").read_text())
cases = meta['cases']; COLS = meta['cols']; CELL = meta['cell']


def stats(sub):
    """该 case 的墨量（偏离背景的总量）与覆盖像素数。"""
    dev = np.abs(sub - bg).sum(axis=2)
    return float(dev[dev > 6].sum()), int((dev > 6).sum())


print(f"{'case':<20}{'霜天墨量':>12}{'浏览器墨量':>12}{'比值':>9}{'覆盖pix差':>11}{'判定':>8}")
rows = []
for i, c in enumerate(cases):
    col, row = i % COLS, i // COLS
    y0 = int(row * CELL * 1.5); y1 = int((row + 1) * CELL * 1.5)
    x0 = int(col * CELL * 1.5); x1 = int((col + 1) * CELL * 1.5)
    a_ink, a_px = stats(st[y0:y1, x0:x1])
    b_ink, b_px = stats(br[y0:y1, x0:x1])
    if b_ink <= 0 and a_ink <= 0:
        verdict, ratio = '都空', 0.0
    elif b_ink <= 0:
        verdict, ratio = '浏览器空', 0.0
    elif a_ink <= 0:
        verdict, ratio = '霜天空', 0.0
    else:
        ratio = a_ink / b_ink
        verdict = 'OK' if 0.9 <= ratio <= 1.1 else ('偏轻' if ratio < 0.9 else '偏重')
    rows.append((c['name'], ratio, verdict))
    print(f"{c['name']:<20}{a_ink:>12.0f}{b_ink:>12.0f}{ratio:>9.3f}{a_px - b_px:>11d}{verdict:>8}")

bad = [r for r in rows if r[2] not in ('OK', '都空')]
print(f"\n总计 {len(rows)} case：{len(rows)-len(bad)} 匹配（±10%），{len(bad)} 需查")
for n, r, v in bad:
    print(f"  ⚠ {n:<20} 比值 {r:.3f}  {v}")
