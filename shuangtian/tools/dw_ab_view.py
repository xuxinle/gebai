#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""真实观感对比图（纯 PIL 拼接，像素 1:1 保真，不用 matplotlib 插值）。

为什么要单独一个脚本：matplotlib 的 imshow 会重采样，字边缘被二次插值，
"看不出真实效果"。这里全用 PIL 直接贴像素，zoom 只用 NEAREST。

产物（build/dwab/figs/）：
  view-1x-light.png   1:1 真实观感（屏幕上是多大就多大），浅底
  view-2x-light.png   2× 放大（看笔画内部是否到满黑）
  view-1x-dark.png    1:1 深色主题
  view-2x-dark.png    2× 深色主题
  view-glyph-4x.png   单字超放大（汉字/拉丁/数字各一组，4×）

用法：python tools/dw_ab_view.py
"""
import json
import pathlib

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
D = ROOT / "build" / "dwab"
FIGS = D / "figs"
FIGS.mkdir(exist_ok=True)

FONT_PATH = "C:/Windows/Fonts/msyh.ttc"
FONT_BOLD = "C:/Windows/Fonts/msyhbd.ttc"


def font(size, bold=False):
    try:
        return ImageFont.truetype(FONT_BOLD if bold else FONT_PATH, size)
    except Exception:
        return ImageFont.load_default()


# 四条路径（浅底）
LIGHT_SOURCES = [
    ("霜天 应用默认", "st-light-default.png"),
    ("霜天 taps 收窄", "stsweep-0_51_205_51_0-g1.0.png"),
    ("DWrite 直调", "dw-natural-light.ppm"),
    ("浏览器 真窗口", "br-clean-light.png"),
]
DARK_SOURCES = [
    ("霜天 应用默认", "st-dark.png"),
    ("DWrite 直调", "dw-natural-dark.ppm"),
]


def load(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.uint8)


def bands_of(img_rgb, fg, bg, min_ink=1.0, gap=2):
    """按覆盖率行投影切带（与量尺同口径）。"""
    t = np.clip(((img_rgb.astype(np.float64) - bg) * (fg - bg)).sum(2) / float(((fg - bg) ** 2).sum()), 0, 1)
    prof = t.sum(1)
    rows = np.where(prof > min_ink)[0]
    if rows.size == 0:
        return [], t
    out, start, prev = [], rows[0], rows[0]
    for y in rows[1:]:
        if y - prev > gap:
            out.append((int(start), int(prev) + 1))
            start = y
        prev = y
    out.append((int(start), int(prev) + 1))
    return out, t


def htrim(t, b):
    col = t[b[0]:b[1]].sum(0)
    c = np.where(col > 0.5)[0]
    return (int(c[0]), int(c[-1]) + 1) if c.size else None


def compose(theme, zoom, out_name, row_labels, sources, bg_rgb, fg_rgb, note):
    """把同一批文本行在多个渲染路径下并排拼成一张图。"""
    imgs, covs, bands = {}, {}, {}
    for name, f in sources:
        p = D / f
        if not p.exists():
            print("跳过（缺文件）", p)
            continue
        imgs[name] = load(p)
        bands[name], covs[name] = bands_of(imgs[name], np.array(fg_rgb, float), np.array(bg_rgb, float))

    # 用最后一列（通常是最"标准"的）确定行带顺序
    ref_name = list(imgs)[-1]
    meta = json.loads((D / f"rows-{theme}.json").read_text(encoding="utf-8"))
    label_index = {m["label"]: i for i, m in enumerate(meta["rows"])}

    label_w = 132
    gutter = 10
    row_gap = 16
    crops = []  # (label, [(name, ndarray)])

    for lab in row_labels:
        li = label_index.get(lab)
        if li is None or li >= len(bands[ref_name]):
            continue
        per = []
        for name in imgs:
            b = bands[name][li]
            t = covs[name]
            h = htrim(t, b)
            if h is None:
                continue
            crop = imgs[name][b[0]:b[1], h[0]:h[1]]
            per.append((name, crop))
        if per:
            crops.append((lab, per))

    # 统一高度（每个文本行内各路径补齐到同样的高度，顶部对齐 = 基线对齐）
    col_w = max(max(c.shape[1] for _, c in per) for _, per in crops)
    col_w = min(col_w, 560)          # 别太宽，留出四列
    prepared = []
    for lab, per in crops:
        hmax = max(c.shape[0] for _, c in per)
        items = []
        for name, c in per:
            pad_b = hmax - c.shape[0]
            cc = np.pad(c, ((0, pad_b), (0, 0), (0, 0)), mode="edge")
            items.append((name, cc))
        prepared.append((lab, items, hmax))

    ncol = len(imgs)
    total_w = label_w + ncol * (col_w * zoom + gutter) + gutter
    head_h = 96
    body_h = sum(h * zoom + row_gap for _, _, h in prepared)
    head2_h = 40
    total_h = head_h + body_h + head2_h

    canvas = Image.new("RGB", (total_w, total_h), tuple(bg_rgb))
    dr = ImageDraw.Draw(canvas)

    # 标题
    dr.text((16, 14), "霜天字体渲染 · 实际观感对比", font=font(26, True),
            fill=tuple(fg_rgb))
    sub = ("同一份文本 / 同一字体（微软雅黑 + Consolas）/ 同一字号与位置；"
           f"本图 {zoom}× 呈现" + ("（像素放大，看笔画内部是否到满黑）" if zoom > 1 else "（屏幕上真实大小）"))
    dr.text((16, 50), sub, font=font(14), fill=(90, 90, 90) if theme == "light" else (170, 170, 170))

    # 列头
    for ci, name in enumerate(imgs):
        x = label_w + ci * (col_w * zoom + gutter)
        dr.text((x, 76), name, font=font(14, True), fill=tuple(fg_rgb))

    y = head_h
    for lab, items, hmax in prepared:
        dr.text((10, y + 2), lab, font=font(12), fill=(120, 120, 120) if theme == "light" else (150, 150, 150))
        for ci, (name, arr) in enumerate(items):
            x = label_w + ci * (col_w * zoom + gutter)
            tile = Image.fromarray(arr).resize((arr.shape[1] * zoom, arr.shape[0] * zoom), Image.NEAREST)
            # 整列底衬（让不同渲染的宽度差看得见边）
            canvas.paste(tile, (x, y))
        y += hmax * zoom + row_gap

    dr.text((16, y + 10), note, font=font(13),
            fill=(110, 110, 110) if theme == "light" else (165, 165, 165))
    canvas.save(FIGS / out_name)
    print("wrote", FIGS / out_name, canvas.size)


def compose_glyph(zoom=8):
    """单字超放大（默认 8×）：一个字的笔画里是灰芯还是实心，一眼可见。"""
    meta = json.loads((D / "rows-light.json").read_text(encoding="utf-8"))
    label_index = {m["label"]: i for i, m in enumerate(meta["rows"])}
    sources = LIGHT_SOURCES
    imgs, bands, covs = {}, {}, {}
    for name, f in sources:
        p = D / f
        if not p.exists():
            continue
        imgs[name] = load(p)
        bands[name], covs[name] = bands_of(imgs[name], np.array([15., 23., 42.]), np.array([255., 255., 255.]))

    # 每行取前几个字（按等宽均分；汉字行是等 advance 的）
    picks = [("han-dense", 2), ("han-body", 2), ("latin-body", 3), ("digits", 4)]
    cells = []
    for lab, nglyph in picks:
        li = label_index[lab]
        text = meta["rows"][li]["text"]
        for name in imgs:
            b = bands[name][li]
            h = htrim(covs[name], b)
            if h is None:
                continue
            full = imgs[name][b[0]:b[1], h[0]:h[1]]
            per = full.shape[1] / max(len(text), 1)      # 每个字的物理宽
            cut = int(round(per * nglyph))
            cells.append((lab, name, full[:, :cut]))

    cw = max(c.shape[1] for _, _, c in cells) * zoom
    ch = max(c.shape[0] for _, _, c in cells) * zoom
    label_w = 124
    sep = 26
    canvas = Image.new("RGB", (label_w + len(imgs) * (cw + sep) + sep,
                               128 + len(picks) * (ch + 44)), (255, 255, 255))
    dr = ImageDraw.Draw(canvas)
    dr.text((16, 14), "单字超放大（8×）：笔画里是“灰芯”还是实心，一眼可辨",
            font=font(25, True), fill=(15, 23, 42))
    dr.text((16, 52), "同一个字、同一字号（15px 逻辑 / 22.5px 物理）。看左两列笔画内部：霜天默认档发灰、笔画间发糊；"
                      "taps 收窄后变实；浏览器最硬。", font=font(13), fill=(90, 90, 90))

    for ci, name in enumerate(imgs):
        dr.text((label_w + ci * (cw + sep), 92), name, font=font(14, True), fill=(15, 23, 42))

    y = 124
    for lab, nglyph in picks:
        dr.text((8, y + ch // 2 - 8), lab, font=font(12), fill=(130, 130, 130))
        for ci, name in enumerate(imgs):
            hit = [c for l, n, c in cells if l == lab and n == name]
            if not hit:
                continue
            arr = hit[0]
            tile = Image.fromarray(arr).resize((arr.shape[1] * zoom, arr.shape[0] * zoom), Image.NEAREST)
            x = label_w + ci * (cw + sep)
            # 交替浅灰底衬 + 竖分隔线，避免跨列误读
            if ci % 2 == 0:
                dr.rectangle([x - 6, y - 6, x + cw + 6, y + ch + 6], fill=(247, 247, 250))
                dr.text((x - 6, y - 6), "", font=font(1), fill=(247, 247, 250))
            canvas.paste(tile, (x, y))
        y += ch + 44
    # 竖分隔线（画在最后，覆盖在底衬上）
    for ci in range(1, len(imgs)):
        x = label_w + ci * (cw + sep) - sep // 2
        dr.line([(x, 88), (x, y - 20)], fill=(225, 225, 230), width=1)
    canvas.save(FIGS / "view-glyph-8x.png")
    print("wrote", FIGS / "view-glyph-8x.png", canvas.size)


def main():
    rows_full = ["han-body", "han-dense", "han-simple", "han-thin",
                 "latin-body", "latin-thin", "digits", "mixed", "mono"]
    rows_sizes = ["size-11", "size-13", "size-16", "size-24"]

    compose("light", 1, "view-1x-light.png", rows_full + rows_sizes, LIGHT_SOURCES,
            (255, 255, 255), (15, 23, 42),
            "1:1 真实观感：霜天默认档（第 1 列）整体偏淡偏软；taps 收窄（第 2 列）笔画明显变实；"
            "浏览器（第 4 列）最硬。")
    compose("light", 2, "view-2x-light.png", rows_full[:6] + rows_sizes, LIGHT_SOURCES,
            (255, 255, 255), (15, 23, 42),
            "2× 放大：注意笔画内部——霜天默认档有灰芯（没到满黑），且笔画边缘摊得宽。")
    compose("dark", 1, "view-1x-dark.png", rows_full + rows_sizes, DARK_SOURCES,
            (15, 17, 21), (232, 238, 249),
            "深色主题（1:1）：白字黑底时方向相反，霜天默认档在深底上比浅底更接近参照。")
    compose("dark", 2, "view-2x-dark.png", rows_full[:6], DARK_SOURCES,
            (15, 17, 21), (232, 238, 249), "深色 2× 放大。")
    compose_glyph(8)
    print("\n全部产物：", FIGS)


if __name__ == "__main__":
    main()
