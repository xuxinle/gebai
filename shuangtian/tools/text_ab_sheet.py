#!/usr/bin/env python3
"""霜天 vs 浏览器 逐行**放大对照图**（人眼确认用）。

每行两块：上 = 霜天，下 = 浏览器；中间一条青线分隔。只做**裁剪 + 最近邻放大**，
不缩放对齐、不调色——插值会伪造锐度，那就没法用眼睛判断了。

两侧行位置不同源（见 `text_ab_diff.py` 口径 1），所以**各自按投影切带、按序配对**，
再按**行带左上角**对齐贴在一起。

用法：python tools/text_ab_sheet.py <name> <dir> [out.png] [--rows=0,1,2] [--zoom=4]
"""
import importlib.util
import json
import pathlib
import sys

import numpy as np
from PIL import Image

TOOLS = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("ab", TOOLS / "text_ab_diff.py")
ab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ab)

GAP = 5          # 上下两块之间的青线宽度
PAD = 3          # 行带上下留白
SEP = (0, 229, 255)


def main() -> int:
    name, directory = sys.argv[1], sys.argv[2]
    out_path = f"/tmp/glyph-ab2/sheet-{name}.png"
    zoom = 4
    rows_arg = None
    for a in sys.argv[3:]:
        if a.startswith("--zoom="):
            zoom = int(a.split("=", 1)[1])
        elif a.startswith("--rows="):
            rows_arg = [int(x) for x in a.split("=", 1)[1].split(",")]
        else:
            out_path = a

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

    pick = rows_arg if rows_arg is not None else list(range(len(expect)))
    tiles = []
    for k in pick:
        m, (sy0, sy1), (by0, by1) = expect[k], bs[k], bb[k]
        top = max(0, min(sy0, by0) - PAD)
        bot = min(h, max(sy1, by1) + PAD)
        # 宽度按该行文字长度估：中英混排下用 0.62 em/字（汉字 1.0 em、拉丁约 0.5）。
        wide = min(w, 16 + int(m["size"] * 1.5 * max(6, len(m["label"])) * 0.72))
        a = st[top:bot, :wide]
        b = br[top:bot, :wide]
        hh = a.shape[0] + b.shape[0] + GAP
        tile = np.full((hh, wide, 3), 255, dtype=np.uint8)
        tile[:a.shape[0]] = a.astype(np.uint8)
        tile[a.shape[0]:a.shape[0] + GAP] = SEP
        tile[a.shape[0] + GAP:] = b.astype(np.uint8)
        tiles.append(tile)
        tiles.append(np.full((2, wide, 3), 205, dtype=np.uint8))

    width = max(t.shape[1] for t in tiles)
    total = sum(t.shape[0] for t in tiles)
    sheet = np.full((total, width, 3), 245, dtype=np.uint8)
    y = 0
    for t in tiles:
        sheet[y:y + t.shape[0], :t.shape[1]] = t
        y += t.shape[0]
    img = Image.fromarray(sheet)
    img = img.resize((img.width * zoom, img.height * zoom), Image.NEAREST)
    img.save(out_path)
    print(f"已写 {out_path}（{img.width}x{img.height}，{len(pick)} 行，每行 上=霜天 下=浏览器）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
