#!/usr/bin/env python3
"""**完整对比总图**：把多套 A/B 页拼成一张（上=霜天、下=浏览器，逐行配对）。

## 为什么要拼总图（而不是各页截图拼一起）

各页各自截图的**行 y 位置不一致**（两侧基线口径不同），而本工具用
`text_ab_diff.py` 的同一套切带与配对逻辑：**每页各侧独立按行投影切带、按序配对**，
再逐行把两侧贴成"上霜天 / 下浏览器"。这样拼出来的每一对都是**同一个内容**，
不会出现"看着对不上"的错觉。

## 排版说明

  · 每页之间留一条宽分隔带（含页名），页内每行之间留细线；
  · 整图按 `--zoom` 最近邻放大（不插值，避免伪造锐度）；
  · 输入目录里需要 `<name>.boxes.json` / `<name>.fonts.json`（由 `text_ab_shot.mjs` 产出）。

用法：python tools/text_ab_sheet_all.py <dir> [out.png] [--zoom=2] [--pages=chars,sizes,...]
"""
import importlib.util
import json
import pathlib
import sys

import numpy as np
from PIL import Image, ImageDraw

TOOLS = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("ab", TOOLS / "text_ab_diff.py")
ab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ab)

PAGE_TITLE = {
    "chars": "① 字符集（全字母数字 · 标点 · 符号 · 简单→复杂汉字 · 颜色/字重 · 等宽）",
    "sizes": "② 字号阶梯（10 ~ 48px，含等宽）",
    "families": "③ 字族 × 字号（同一字族单独成行：拉丁 / 汉字 / 等宽）",
    "glyphs": "④ 逐字形（一行一字符，11 / 13 / 15px —— 用于核「大写 vs 小写」的笔画宽度）",
}
SEP = (0, 229, 255)
GAP = 4
PAD = 2


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(float)


def render_page(d: pathlib.Path, name: str, zoom: int) -> Image.Image | None:
    meta = json.loads((TOOLS / f"text_ab_{name}_rows.json").read_text())["rows"]
    expect = [m for m in meta if m["label"] != "（空行）"]
    st = load(d / f"ab-{name}-st.png")
    br = load(d / f"ab-{name}-br.png")
    h = min(st.shape[0], br.shape[0])
    w = min(st.shape[1], br.shape[1])
    st, br = st[:h, :w], br[:h, :w]
    bs, bb = ab.bands(st), ab.bands(br)
    if not (len(bs) == len(bb) == len(expect)):
        print(f"  [{name}] 带数不符 {len(bs)}/{len(bb)}/{len(expect)} —— 跳过该页")
        return None

    tiles = []
    for m, (sy0, sy1), (by0, by1) in zip(expect, bs, bb):
        top = max(0, min(sy0, by0) - PAD)
        bottom = min(h, max(sy1, by1) + PAD)
        # 宽度按该行文字长度估（汉字≈1em、拉丁≈0.55em，取 0.7 折中 + 余量）
        wide = min(w, 24 + int(m["size"] * 1.5 * max(6, len(m["label"])) * 0.7))
        a = st[top:bottom, :wide].astype(np.uint8)
        b = br[top:bottom, :wide].astype(np.uint8)
        tile = np.full((a.shape[0] + b.shape[0] + GAP, wide, 3), 255, dtype=np.uint8)
        tile[:a.shape[0]] = a
        tile[a.shape[0]:a.shape[0] + GAP] = SEP
        tile[a.shape[0] + GAP:] = b
        tiles.append(tile)
        tiles.append(np.full((2, wide, 3), 205, dtype=np.uint8))

    width = max(t.shape[1] for t in tiles)
    # 页标题带
    header = np.full((30, width, 3), 28, dtype=np.uint8)
    total = sum(t.shape[0] for t in tiles) + header.shape[0]
    sheet = np.full((total, width, 3), 245, dtype=np.uint8)
    sheet[:header.shape[0]] = header
    y = header.shape[0]
    for t in tiles:
        sheet[y:y + t.shape[0], :t.shape[1]] = t
        y += t.shape[0]
    img = Image.fromarray(sheet)
    draw = ImageDraw.Draw(img)
    draw.text((8, 9), f"{PAGE_TITLE.get(name, name)}   ★ 每对：上 = 霜天，下 = 浏览器", fill=(255, 255, 255))
    if zoom != 1:
        img = img.resize((img.width * zoom, img.height * zoom), Image.NEAREST)
    return img


def main() -> int:
    d = pathlib.Path(sys.argv[1])
    out = "/tmp/ab-full/sheet-full.png"
    zoom = 2
    pages = ["chars", "sizes", "families", "glyphs"]
    for a in sys.argv[2:]:
        if a.startswith("--zoom="):
            zoom = int(a.split("=", 1)[1])
        elif a.startswith("--pages="):
            pages = a.split("=", 1)[1].split(",")
        else:
            out = a

    images = []
    for name in pages:
        if not (d / f"ab-{name}-st.png").exists():
            print(f"  [{name}] 缺渲染产物 —— 跳过")
            continue
        img = render_page(d, name, zoom)
        if img is not None:
            images.append((name, img))
    if not images:
        print("没有可拼的页")
        return 1

    width = max(img.width for _, img in images)
    gap = 26 * zoom
    total = sum(img.height for _, img in images) + gap * (len(images) - 1)
    sheet = Image.new("RGB", (width, total), (232, 232, 238))
    y = 0
    for _, img in images:
        sheet.paste(img, (0, y))
        y += img.height + gap
    sheet.save(out)
    print(f"已写 {out}（{sheet.width}x{sheet.height}，{len(images)} 页："
          f"{', '.join(n for n, _ in images)}）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
