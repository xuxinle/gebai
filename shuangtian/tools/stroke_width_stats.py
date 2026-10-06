#!/usr/bin/env python3
"""**逐字形竖笔宽度对照**（霜天 vs 浏览器）——回答"H/1/B 偏粗、a/n/l 偏细"这类反馈。

## 为什么必须逐字形，而且一行一个字

整行平均会把大写与小写的笔宽混在一起，两者方向相反时正好抵消——用户看到的现象
在那个尺子里**根本不存在**。而按列投影切字形在小字号下会粘连（实测 11px 下 23 个字符
只切出 7~9 段），切错之后按序配对全错位，量出的"笔宽比"是假的。
所以对照页做成**一行一个字符**（`tools/text_ab_glyphs_page.html`，63 行）。

## 取哪一条横截面（两次踩到的坑）

1. **不能用自动切带**：`i`/`j` 的点与竖之间有空行，会被切成两带（实测 63 行切出 66 带），
   一带错后面全部错位 ⇒ 用浏览器的 **DOM 实测行框**，取相邻行框中点作上下界。
2. **不能在竖直中线取横截面**：`H`/`E`/`F`/`A` 的横杠/中横正好在中线，会把
   "横杠 + 两根竖"量成一条宽得离谱的笔画（实测 H 在 11px 下被量成 5.41px）。
   ⇒ 取 **高度 22%/30%/70%/78% 四处**（避开横杠与 B 的碗接合处），每处取该处最粗的连续段，
   再对这四处的值取**中位数**。

## 判据

同一字号下**大写与小写的笔宽比**应当接近参照（同一字体同一设计值）。
霜天若显著偏离（实测 11px 默认档曾达 1.56，参照 1.10），就是机制在按字形差别对待。

用法：python tools/stroke_width_stats.py <dir> <name> [--only=H1Banl] [--size=11] [--side=st|br]
"""
import json
import pathlib
import sys

import numpy as np
from PIL import Image

TOOLS = pathlib.Path(__file__).resolve().parent
BG = np.array([255.0, 255.0, 255.0])
# 取横截面的高度比例：避开横杠（中线）与 B/S 的碗接合处
FRACTIONS = (0.22, 0.30, 0.70, 0.78)
DEFAULT_CHARS = ("H", "E", "F", "T", "L", "I", "B", "n", "l", "i", "r", "t")


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(float)


def coverage(sub, fg):
    d = BG - np.array(fg, dtype=float)
    return np.clip(((BG - sub) @ d) / float(d @ d), 0.0, 1.0)


def stems_in_row(row, min_cov=0.05):
    """该扫描行里的连续覆盖率段（每段 = 一根笔画），返回各段宽度（覆盖率之和）。"""
    x = np.where(row > min_cov)[0]
    if x.size == 0:
        return []
    seg = row[x[0]:x[-1] + 1]
    out, k = [], 0
    while k < len(seg):
        if seg[k] > min_cov:
            start = k
            while k < len(seg) and seg[k] > min_cov:
                k += 1
            out.append(float(seg[start:k].sum()))
        else:
            k += 1
    return out


def main() -> int:
    d = pathlib.Path(sys.argv[1])
    name = sys.argv[2]
    only, size_filter, side = None, None, "st"
    for a in sys.argv[3:]:
        if a.startswith("--only="):
            only = a.split("=", 1)[1]
        elif a.startswith("--size="):
            size_filter = float(a.split("=", 1)[1])
        elif a.startswith("--side="):
            side = a.split("=", 1)[1]

    meta = json.loads((TOOLS / f"text_ab_{name}_rows.json").read_text())["rows"]
    exp = [r for r in meta if r["label"] != "（空行）"]
    boxes = json.loads((d / f"{name}.boxes.json").read_text())
    img = load(d / f"ab-{name}-{side}.png")

    def stems_of(ch, size):
        i = next((k for k, m in enumerate(exp)
                  if m["label"].split()[-1] == ch and m["size"] == size), None)
        if i is None:
            return []
        lo = 0 if i == 0 else (boxes[i - 1][2] + boxes[i][1]) // 2
        hi = img.shape[0] if i + 1 >= len(boxes) else (boxes[i][2] + boxes[i + 1][1]) // 2
        cov = coverage(img[lo:hi, :], tuple(int(exp[i]["color"][k:k + 2], 16) for k in (1, 3, 5)))
        rows = np.where((cov > 0.5).any(axis=1))[0]
        if rows.size == 0:
            return []
        cov = cov[rows[0]:rows[-1] + 1]
        height = cov.shape[0]
        per_row = []
        for f in FRACTIONS:
            per_row += stems_in_row(cov[max(0, min(height - 1, int(height * f)))])
        return per_row

    print(f"{'字符':<5}{'类':<5}{'字号':>5}{'竖笔宽（中位）':>16}{'各截面':>28}")
    print("-" * 62)
    groups = {}
    for size in sorted({m["size"] for m in exp}):
        if size_filter is not None and size != size_filter:
            continue
        for ch in DEFAULT_CHARS:
            if only is not None and ch not in only:
                continue
            v = stems_of(ch, size)
            if not v:
                continue
            w = float(np.median(v))
            cls = "大写" if ch.isupper() else ("小写" if ch.islower() else "数字")
            groups.setdefault((cls, size), []).append(w)
            print(f"{ch:<5}{cls:<5}{size:>5g}{w:>16.2f}"
                  f"{'  ' + ' '.join(f'{x:.2f}' for x in v[:4]):>28}")
    print("-" * 62)
    print(f"{'类':<6}{'字号':>5}{'n':>4}{'中位':>9}")
    for (cls, size), v in sorted(groups.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        print(f"{cls:<6}{size:>5g}{len(v):>4}{np.median(v):>9.2f}")
    for size in sorted({s for _, s in groups}):
        up = groups.get(("大写", size))
        low = groups.get(("小写", size))
        if up and low:
            print(f"→ {size:g}px 大写/小写 笔宽比 = {np.median(up) / np.median(low):.3f}"
                  f"（参照侧的同一比值应≈1.0~1.1）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
