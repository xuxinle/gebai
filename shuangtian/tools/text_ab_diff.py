#!/usr/bin/env python3
"""文字渲染 A/B 逐行量化对比（霜天 vs 浏览器）——行表来自生成器的同一张表。

## 四条硬口径（每一条都是踩过坑之后定下来的）

1. **两侧各自按行投影切带，再按序配对**。
   两侧的**行 y 位置本身就不同源**：浏览器 `top:` 是行盒顶边、基线在 `ascent` 处
   （Noto Sans CJK 约 0.88 em）；霜天 `draw(origin)` 的 origin 是文本框顶、基线在
   **字体 ascender** 处（同一字体是 1.16 em，@15px/1.5 实测低 **7.8 物理像素**）。
   于是"按浏览器行框切同一个 y 带"会同时**截掉霜天字形的下部、切进下一行的上部**
   ——实测把汉字墨量比读成 0.41~0.65（逐字形探针核对真值约 0.90），并据此
   得出**方向相反**的结论（"霜天 CFF 少墨"，而真实原因是量尺错位）。
   ⇒ 每侧独立切带（行间本来就有空白行隔开），再按序配对；带数不符即报错。
2. **覆盖率逐通道投影**到 `[白底, 该行前景色]` 连线上：亚像素渲染下 R/G/B 被不同强度点亮，
   只看亮度会把"彩边"读成"覆盖率低"。前景色**逐行取**（用正文色量 muted/faint
   会把"颜色本来就浅"读成"没到满黑"）。
3. **水平方向各自紧裁**（排版外延/字距取整差异不属于"字形渲染"），垂直方向用切带结果，
   并报两侧带高/带宽比（1.0 = 同尺寸）。
4. **先断言两侧字体同一套**：浏览器侧实际字体由 CDP 导出（`<name>.fonts.json`），
   与霜天侧字体链比对；不一致直接退出——否则量到的是字体差异不是渲染差异。

用法：python tools/text_ab_diff.py <name> <dir>
  文件：<dir>/ab-<name>-st.png、ab-<name>-br.png、<name>.fonts.json、<name>.boxes.json
"""
import json
import pathlib
import sys

import numpy as np
from PIL import Image

TOOLS = pathlib.Path(__file__).resolve().parent
BG = np.array([255.0, 255.0, 255.0])
BAND_TH = 10.0     # 某行任一处偏离白底超过它即算"有墨"
INK_TH = 0.12      # 覆盖率阈值（用于紧裁与统计）
ST_FACES = {"DejaVu Sans", "Noto Sans CJK SC", "DejaVu Sans Mono"}


def hex_rgb(s: str):
    return tuple(int(s[i:i + 2], 16) for i in (1, 3, 5))


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(float)


def coverage(sub, fg):
    d = BG - np.array(fg, dtype=float)
    return np.clip(((BG - sub) @ d) / float(d @ d), 0.0, 1.0)


def bands(img, gap=2):
    """按行投影切带：任一处偏离白底 > BAND_TH 即视为该行有墨；空白超过 `gap` 行才断带。"""
    dev = (255.0 - img).max(axis=2)           # 逐像素最大通道偏离
    on = (dev > BAND_TH).any(axis=1)
    out, start, blanks = [], None, 0
    for y, flag in enumerate(on):
        if flag:
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
    return out


def tight(c):
    rows = np.where((c > INK_TH).any(axis=1))[0]
    cols = np.where((c > INK_TH).any(axis=0))[0]
    if rows.size == 0 or cols.size == 0:
        return None
    return c[rows[0]:rows[-1] + 1, cols[0]:cols[-1] + 1]


def stats(c):
    return int((c > 0.85).sum()), int(((c > 0.15) & (c < 0.85)).sum()), float(c.sum())


def main() -> int:
    name, directory = sys.argv[1], sys.argv[2]
    d = pathlib.Path(directory)
    meta = json.loads((TOOLS / f"text_ab_{name}_rows.json").read_text())["rows"]
    fonts = json.loads((d / f"{name}.fonts.json").read_text())

    used = {f["family"] for row in fonts for f in row["faces"]}
    if used - ST_FACES:
        print(f"⚠ 浏览器实际用到霜天没有的字体：{sorted(used - ST_FACES)}")
        print(f"  浏览器：{sorted(used)}；霜天链：{sorted(ST_FACES)}")
        return 2
    print(f"字体一致：两侧均为 {sorted(used)}")

    st_img, br_img = load(d / f"ab-{name}-st.png"), load(d / f"ab-{name}-br.png")
    h = min(st_img.shape[0], br_img.shape[0])
    w = min(st_img.shape[1], br_img.shape[1])
    st_img, br_img = st_img[:h, :w], br_img[:h, :w]

    expect = [m for m in meta if m["label"] != "（空行）"]
    bs, bb = bands(st_img), bands(br_img)
    print(f"切带：霜天 {len(bs)} 条 / 浏览器 {len(bb)} 条（期望 {len(expect)}）")
    if len(bs) != len(bb) or len(bs) != len(expect):
        print("带数不符——先确认对照页布局与两侧行距（行距必须大于字高，否则相邻行会连成一带）")
        for tag, arr in (("霜天", bs), ("浏览器", bb)):
            print(f"  {tag}: {[(a, b, b - a) for a, b in arr]}")
        return 1

    print(f"{'行':<22}{'字号':>5}{'带高 霜/浏':>12}{'宽 霜/浏':>12}"
          f"{'实心 霜/浏':>14}{'过渡 霜/浏':>14}{'墨量比':>8}{'显著差':>8}")
    print("-" * 98)
    rows = []
    for m, (sy0, sy1), (by0, by1) in zip(expect, bs, bb):
        fg = hex_rgb(m["color"])
        ta = tight(coverage(st_img[sy0:sy1, :], fg))
        tb = tight(coverage(br_img[by0:by1, :], fg))
        if ta is None or tb is None:
            print(f"{m['label'][:20]:<22}—— 一侧无墨迹")
            continue
        ww = min(ta.shape[1], tb.shape[1])
        hh = min(ta.shape[0], tb.shape[0])
        a2, b2 = ta[:hh, :ww], tb[:hh, :ww]
        sa, ma, ia = stats(a2)
        sb, mb, ib = stats(b2)
        sig = float((np.abs(a2 - b2) > 0.15).mean()) * 100.0
        rows.append((m["label"], m["size"], ta.shape[0], tb.shape[0], ta.shape[1], tb.shape[1],
                     sa, sb, ma, mb, ia, ib, sig))
        print(f"{m['label'][:20]:<22}{m['size']:>5g}"
              f"{ta.shape[0]:>6}/{tb.shape[0]:<6}{ta.shape[1]:>5}/{tb.shape[1]:<7}"
              f"{sa:>7}/{sb:<7}{ma:>7}/{mb:<7}"
              f"{(ia / ib if ib else 0):>8.2f}{sig:>7.1f}%")
    if not rows:
        return 1
    ta_, tb_ = sum(r[10] for r in rows), sum(r[11] for r in rows)
    print("-" * 98)
    print(f"合计墨量：霜天 {ta_:.0f} / 浏览器 {tb_:.0f}   比值 {ta_ / tb_ if tb_ else 0:.3f}")
    print(f"平均显著差异：{np.mean([r[12] for r in rows]):.1f}%   "
          f"中位数 {np.median([r[12] for r in rows]):.1f}%")
    print(f"带高比 中位 {np.median([r[2] / r[3] for r in rows]):.3f}   "
          f"带宽比 中位 {np.median([r[4] / r[5] for r in rows]):.3f}（1.0 = 同尺寸）")
    print("墨量比最偏的 6 行：")
    for r in sorted(rows, key=lambda r: -(abs(r[10] / r[11] - 1.0) if r[11] else 0))[:6]:
        print(f"  {r[0][:22]:<24}{r[1]:>4g}px  墨量比 {r[10] / r[11] if r[11] else 0:.2f}   "
              f"过渡/实心 霜天 {r[8] / r[6] if r[6] else 0:.3f}"
              f"  浏览 {r[9] / r[7] if r[7] else 0:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
