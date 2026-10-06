#!/usr/bin/env python3
"""**值不值得做**：提示机制能给数字/字母补回多少？（上限测量）

## 要回答的问题

实测霜天在 TrueType 字体（DejaVu）上比浏览器轻 6~9%（数字 8~10%），
归因是"浏览器执行字体自带的提示指令，霜天不执行"。但做 TT 指令解释器代价很大
（`fpgm`/`prep`/glyf 指令集 + 图形状态 + CVT + 函数定义），所以先量**上限**。

## 口径：**原始墨量和**，不用覆盖率投影

投影口径（把像素投到 [白底, 前景] 连线上）**要求两侧的抗锯齿链路可比**——
而 FreeType 的 LCD 位图是**未滤波**的、霜天/浏览器是**滤波**的，彩边分布不同，
投影会把差异读成"墨量差"（实测过一次：灰度渲染下"FT 关提示/浏览器"≈1.02，
换上 LCD 又变成 1.02~1.05，而逐字形原始墨量和是 0.985~1.002——同一件事三个答案）。
⇒ 本工具改用**逐字形原始墨量和** `Σ(255−channel)/255/3`（与
`tools/freetype_ink_decompose.cpp` 完全同一口径），它不含任何空间假设。

## 输入

`<ft_dir>/ft-hint-on.png`、`ft-hint-off.png`（由 `tools/freetype_glyphs_render.cpp` 渲染）、
浏览器截图、以及同一份行表 JSON。

用法：python tools/freetype_hint_ceiling.py <ft_dir> <browser_png> <rows.json>
"""
import importlib.util
import json
import pathlib
import sys

import numpy as np

TOOLS = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("ab", TOOLS / "text_ab_diff.py")
ab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ab)


def ink_map(img, meta, boxes_key="boxes"):
    """逐字形原始墨量和（行窗口取相邻行框中点之间，与 `stroke_width_stats.py` 同口径）。"""
    exp = [m for m in meta if m["label"] != "（空行）"]
    bands = ab.bands(img)
    if len(bands) != len(exp):
        return None, len(bands), len(exp)
    out = {}
    for m, (y0, y1) in zip(exp, bands):
        ch = m["label"].split()[-1]
        cls = "数字" if ch.isdigit() else ("大写" if ch.isupper() else "小写")
        sub = img[y0:y1, :, :]
        # 墨迹紧裁（水平 + 垂直），再算墨量和——与逐字形探针同口径
        dev = (255.0 - sub).max(axis=2)
        rows = np.where((dev > 24).any(axis=1))[0]
        cols = np.where((dev > 24).any(axis=0))[0]
        if rows.size == 0 or cols.size == 0:
            continue
        box = sub[rows[0]:rows[-1] + 1, cols[0]:cols[-1] + 1, :]
        ink = float((255.0 - box).sum()) / 255.0 / 3.0
        out[(int(m["size"]), cls, ch)] = ink
    return out, len(bands), len(exp)


def ratios(a_map, b_map):
    out = {}
    for key, value in a_map.items():
        if key in b_map and b_map[key] > 0:
            out[key] = value / b_map[key]
    return out


def cluster(ratio_map):
    out = {}
    for (size, cls, _ch), value in ratio_map.items():
        out.setdefault((size, cls), []).append(value)
    return {k: float(np.median(v)) for k, v in out.items()}


def main() -> int:
    ft_dir = pathlib.Path(sys.argv[1])
    browser_png = sys.argv[2]
    rows_json = pathlib.Path(sys.argv[3])
    meta = json.loads(rows_json.read_text())["rows"]

    browser = ab.load(browser_png)
    on = ab.load(ft_dir / "ft-hint-on.png")
    off = ab.load(ft_dir / "ft-hint-off.png")

    b_map, nb, ne = ink_map(browser, meta)
    on_map, no, _ = ink_map(on, meta)
    off_map, nf, _ = ink_map(off, meta)
    print(f"切带：浏览器 {nb} / FT开 {no} / FT关 {nf}（期望 {ne}）")
    if b_map is None or on_map is None or off_map is None:
        print("带数不符：先确认两侧用的是**同一份行表**、同一画布尺寸")
        return 1

    r_off = cluster(ratios(off_map, b_map))
    r_on = cluster(ratios(on_map, b_map))
    r_pair = cluster(ratios(on_map, off_map))

    print(f"\n{'字号':>4}{'类':<6}{'FT关提示/浏览器':>16}{'FT开提示/浏览器':>16}"
          f"{'提示能补回':>12}{'开/关':>8}")
    print("-" * 64)
    for key in sorted(r_off):
        off_v, on_v = r_off[key], r_on.get(key, float("nan"))
        print(f"{key[0]:>4}{key[1]:<6}{off_v:>16.3f}{on_v:>16.3f}"
              f"{on_v - off_v:>+12.3f}{r_pair.get(key, float('nan')):>8.3f}")
    print("\n解读：")
    print("  · 'FT关提示/浏览器' ≈ 霜天现状（霜天不执行提示）——它偏离 1 的多少，")
    print("    就是'不做 TT 解释器'要付的代价；")
    print("  · 'FT开提示/浏览器' = 完全复刻 FreeType 提示后的**上限**（1.0 为理想）；")
    print("  · '提示能补回' = 做 TT 解释器**最多**能把霜天往上抬多少。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
