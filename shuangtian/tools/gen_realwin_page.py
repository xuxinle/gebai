#!/usr/bin/env python3
"""生成**深色主题**真窗口对照页：一框一行、框内深底浅字。

## 为什么深色要单独标

覆盖率预校正的作用是 `α' = 1 − (1−α)^(1/γ)`，这个式子的方向是按**黑字白底**推导的
（γ<1 让中间调更黑、γ>1 更浅）。白字黑底时，"看起来的粗细"由**反方向**的对比决定，
所以浅色标出来的 γ 不能直接拿来用——必须单独量。

底色/字色取主题 token 原值（`src/ui/theme.cpp` 的 `Theme::dark()`）：
  bg `#0A0F1A` / text `#E8EEF9`。

用法：python tools/gen_realwin_page.py --dark [字号...]
      输出 build/probe/realwin/sizes_boxed_dark.html
"""

import pathlib
import sys

SIZES_DEFAULT = [10, 11, 12, 14, 16, 20, 24, 32]
SAMPLE = "组件画廊 Overview 24"
W, H, TOP, GAP = 640, 96, 8, 12
# 深色主题 token（src/ui/theme.cpp Theme::dark）
BG_DARK = "#0A0F1A"
FG_DARK = "#E8EEF9"
FAMILY = "'Segoe UI','Microsoft YaHei'"
# 框边线用**青色**：深底上红色边框会与主题的 warm 色混，且青色在暗底上更易被
# 阈值分离（行投影靠它区分"内容起点"）。
EDGE = "#00E5FF"


def main() -> int:
    args = sys.argv[1:]
    dark = "--dark" in args
    sizes = [int(a) for a in args if not a.startswith("--")] or SIZES_DEFAULT
    root = pathlib.Path(__file__).resolve().parent.parent
    out = root / "build" / "probe" / "realwin"
    out.mkdir(parents=True, exist_ok=True)
    bg = BG_DARK if dark else "#FFFFFF"
    fg = FG_DARK if dark else "#0F172A"
    edge = EDGE if dark else "#FF0000"
    rows, y = [], 0
    for size in sizes:
        rows.append(
            f'<div class="box" id="b{size}" style="top:{y}px">'
            f'<div class="t" style="top:{TOP}px;font-size:{size}px">{SAMPLE}</div></div>')
        y += H + GAP
    html = f"""<!doctype html>
<!-- 由 tools/gen_realwin_page.py 生成——**不要手改**。
     每个框 {W}×{H}、{edge} 描边（供截图后判定内容起点），框内一行字。 -->
<meta charset="utf-8">
<style>
  html, body {{ margin: 0; padding: 0; background: {bg}; }}
  body {{ position: relative; width: {W}px; }}
  /* 框必须显式给宽高：子元素是 absolute、不撑开父容器 */
  .box {{ position: relative; width: {W - 2}px; height: {H - 2}px;
          border: 1px solid {edge}; overflow: hidden; background: {bg}; }}
  .t {{ position: absolute; left: 3px; white-space: nowrap; line-height: 1;
        color: {fg}; font-family: {FAMILY}; }}
</style>
{chr(10).join(rows)}
"""
    name = "sizes_boxed_dark.html" if dark else "sizes_boxed.html"
    (out / name).write_text(html, encoding="utf-8", newline="\n")
    print(f"已生成 {name}：{len(sizes)} 个框（字号 {sizes}），底 {bg} 字 {fg}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
