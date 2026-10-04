#!/usr/bin/env python3
"""生成**真窗口浏览器**对照页：每个字号一个 640×96 的框，框有纯红描边。

## 为什么要有红描边

真窗口截图里，浏览器工具栏/滚动条会混进来，而我要积分的是**每行字所在的那个 640×96 框**。
红描边让框的位置可以**从图里反解**（找纯红像素行列），不依赖任何窗口几何假设——
窗口位置、工具栏高度、滚动条全都不影响。

## 为什么真窗口

DESIGN 明文要求：默认值必须用**真机（非无头）参照**量过。headless Chromium 的字体栈与
真窗口不同（ClearType 调校、子像素开关），实测两者并不等价。

用法：python tools/gen_realwin_page.py [字号...]
      输出 build/probe/realwin/sizes_boxed.html
"""

import pathlib
import sys

SIZES = [int(a) for a in sys.argv[1:]] or [10, 11, 12, 14, 16, 20, 24, 32]
SAMPLE = "组件画廊 Overview 24"
W, H, TOP, GAP = 640, 96, 8, 12
COLOR = "#0F172A"
FAMILY = "'Segoe UI','Microsoft YaHei'"
CRIMSON = "#FF0000"


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    out = root / "build" / "probe" / "realwin"
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    y = 0
    for size in SIZES:
        rows.append(
            f'<div class="box" id="b{size}" style="top:{y}px">'
            f'<div class="t" style="top:{TOP}px;font-size:{size}px">{SAMPLE}</div></div>')
        y += H + GAP
    html = f"""<!doctype html>
<!-- 由 tools/gen_realwin_page.py 生成——**不要手改**。
     每个框 640×96、纯红描边（供截图后反解框位置），框内一行字。 -->
<meta charset="utf-8">
<style>
  html, body {{ margin: 0; padding: 0; background: #FFFFFF; }}
  body {{ position: relative; width: {W}px; }}
  /* 框必须显式给宽高：子元素是 absolute、不撑开父容器 */
  .box {{ position: relative; width: {W - 2}px; height: {H - 2}px;
          border: 1px solid {CRIMSON}; overflow: hidden; }}
  .t {{ position: absolute; left: 3px; white-space: nowrap; line-height: 1;
        color: {COLOR}; font-family: {FAMILY}; }}
</style>
{chr(10).join(rows)}
"""
    (out / "sizes_boxed.html").write_text(html, encoding="utf-8", newline="\n")
    print(f"已生成 sizes_boxed.html：{len(SIZES)} 个框（字号 {SIZES}），"
          f"总高 {y}px，框 {W}×{H}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
