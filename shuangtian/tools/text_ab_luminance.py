#!/usr/bin/env python3
"""**朴素亮度口径**的 A/B 核验：分离"更黑"与"覆盖更多"。

## 为什么需要这把尺子（与 `text_ab_diff.py` 互补）

`text_ab_diff.py` 用**覆盖率投影**口径（把像素投影到 [背景, 前景] 连线上）——
它回答"这层墨有多厚"，但隐含一个空间假设，而亚像素渲染下 R/G/B 被不同强度点亮，
投影会把"彩边"也算成覆盖。

本脚本改用量级更朴素的三件事（**不做任何反解、不带空间假设**）：

| 量 | 定义 | 回答 |
|---|---|---|
| **墨迹面积** | 共享掩码里 Y < 阈值 的像素数（**两侧同一掩码**） | 覆盖得更多吗？ |
| **每墨像素暗度** | 掩码内 `255 − Y` 的**均值** | 同样的覆盖下更黑吗？（= 过度加墨） |
| **墨迹质量** | 行带内 `Σ(255 − Y)` | 总墨量（与前两个量互为分解） |

三个量的关系：`墨迹质量 ≈ 墨迹面积 × 每墨像素暗度`。**只有这样拆开**才能回答
BACKLOG P1 那条"真型字体偏重 1.10~1.16"到底是"字画粗了"还是"墨压黑了"——
两者的修法完全不同（前者改几何/加墨，后者改覆盖率映射）。

⚠ **掩码必须两侧共用**（取并集），否则"自己只算自己的墨"会把面积差异重复计入暗度
（实测：各自取掩码时面积比与暗度比会同时抬高，读起来像"两处都偏重"）。

用法：python tools/text_ab_luminance.py <name> <dir> [--class cjk|latin|mono] [--th 200]
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


def luminance(img):
    """BT.601 亮度（朴素口径：不做 sRGB/线性化假设）。"""
    return 0.299 * img[:, :, 0] + 0.587 * img[:, :, 1] + 0.114 * img[:, :, 2]


def class_of(label, mono):
    if any('\u4e00' <= ch <= '\u9fff' for ch in label):
        return "cjk"
    return "mono" if mono else "latin"


def main() -> int:
    name, directory = sys.argv[1], sys.argv[2]
    wanted = None
    threshold = 200.0
    for a in sys.argv[3:]:
        if a.startswith("--class="):
            wanted = a.split("=", 1)[1]
        elif a.startswith("--th="):
            threshold = float(a.split("=", 1)[1])
    d = pathlib.Path(directory)
    meta = json.loads((TOOLS / f"text_ab_{name}_rows.json").read_text())["rows"]
    st = ab.load(d / f"ab-{name}-st.png")
    br = ab.load(d / f"ab-{name}-br.png")
    h = min(st.shape[0], br.shape[0])
    w = min(st.shape[1], br.shape[1])
    st, br = st[:h, :w], br[:h, :w]
    expect = [m for m in meta if m["label"] != "（空行）"]
    bs, bb = ab.bands(st), ab.bands(br)
    if not (len(bs) == len(bb) == len(expect)):
        print(f"带数不符：{len(bs)}/{len(bb)}/{len(expect)}")
        return 1

    print(f"阈值 Y<{threshold:g}；墨迹面积 = 掩码内暗像素数（两侧**同一掩码**）\n")
    print(f"{'行':<22}{'类':>6}{'面积 霜/浏':>14}{'每墨像素暗度 霜/浏':>22}"
          f"{'面积比':>9}{'暗度比':>9}{'质量比':>9}")
    print("-" * 96)
    rows = []
    for m, (sy0, sy1), (by0, by1) in zip(expect, bs, bb):
        cls = class_of(m["label"], m["mono"])
        if wanted is not None and cls != wanted:
            continue
        # **两侧行带高度必须一致**：`bands` 各自切出来的高度可以差 1 行（两侧基线口径
        # 不同、空白判定边界落在不同行上）。不一致时 `mask` 无法广播——
        # 实测直接抛 ValueError。取两者较小高度（只比共有的那部分墨迹）。
        height = min(sy1 - sy0, by1 - by0)
        ya = luminance(st[sy0:sy0 + height, :])
        yb = luminance(br[by0:by0 + height, :])
        # 水平紧裁：用暗度列投影取墨迹跨度，再按左缘对齐（只比字形，不比排版外延）。
        da = (255.0 - ya).sum(axis=0)
        db = (255.0 - yb).sum(axis=0)
        ia = np.where(da > 1.0)[0]
        ib = np.where(db > 1.0)[0]
        if ia.size == 0 or ib.size == 0:
            continue
        width = min(ia[-1] - ia[0] + 1, ib[-1] - ib[0] + 1)
        a = ya[:, ia[0]:ia[0] + width].ravel()
        b = yb[:, ib[0]:ib[0] + width].ravel()
        mask = (a < threshold) | (b < threshold)     # **共享掩码**
        na, nb = int((mask & (a < threshold)).sum()), int((mask & (b < threshold)).sum())
        if na == 0 or nb == 0:
            continue
        dark_a = float((255.0 - a[mask]).mean())
        dark_b = float((255.0 - b[mask]).mean())
        mass_a, mass_b = float((255.0 - a).sum()), float((255.0 - b).sum())
        area_r = na / nb
        dark_r = dark_a / dark_b if dark_b else 0.0
        rows.append((m["label"], cls, area_r, dark_r, mass_a / mass_b if mass_b else 0))
        print(f"{m['label'][:20]:<22}{cls:>6}{na:>6}/{nb:<8}{dark_a:>9.1f}/{dark_b:<9.1f}"
              f"{area_r:>9.3f}{dark_r:>9.3f}{mass_a / mass_b if mass_b else 0:>9.3f}")

    if not rows:
        return 1
    print("-" * 96)
    for cls in ("cjk", "latin", "mono"):
        sub = [r for r in rows if r[1] == cls]
        if not sub:
            continue
        print(f"{cls:<6} 行数 {len(sub):<3}  面积比 中位 {np.median([r[2] for r in sub]):.3f}"
              f"   暗度比 中位 {np.median([r[3] for r in sub]):.3f}"
              f"   质量比 中位 {np.median([r[4] for r in sub]):.3f}")
    print("\n解读：**面积比 > 1 而暗度比 ≈ 1** ⇒ 覆盖得更多（几何/加墨层面）；")
    print("      **暗度比 > 1** ⇒ 同样的覆盖下更黑（覆盖率映射层面，gamma/对比度）。")
    print("      质量比是两者的乘积，单看它分不清是哪个。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
