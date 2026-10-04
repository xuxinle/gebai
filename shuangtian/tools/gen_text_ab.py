#!/usr/bin/env python3
"""从**单一真源**生成文字 A/B 对照的两侧文件（HTML + 探针的 C++ 行表）。

## 为什么要有这个生成器

对照的成立前提是两侧**逐字、逐字号、逐颜色、逐位置**完全一致。手工维护两份表必然漂移
（第一版就漂了：HTML 用旧字号、探针用另一套）——而漂移导致的差异会被读成"渲染差异"。
所以行表只写在**本文件**里，两侧由它生成。

## 行距为什么必须够大

行带切分靠"空白行"分隔。行距如果只比字高略大，相邻行的上伸部/下伸部会连成一片
（实测 22px 行距 / 15px 字号 → 28 行只切出 12~17 带），后续的按序配对就全错位。
本生成器统一用 `LINE_PITCH`，并保证它 > 最大字号 × 1.5。

用法：python tools/gen_text_ab.py
      输出 tools/text_ab_page.html 与 tools/text_ab_page_rows.inc
"""

import io
import pathlib

# 前景色（主题 token 原值，src/ui/theme.cpp）
TEXT = "#0F172A"
MUTED = "#56647C"
FAINT = "#66768C"
PRIMARY = "#2563EB"
SANS = "'Segoe UI','Microsoft YaHei'"
MONO = "'Cascadia Mono',Consolas,monospace"

# (文本, 字号逻辑px, 前景色, 字体族, 粗体, 分组标题)
ROWS = [
    ("中文标题：组件画廊·资源管理器", 15, TEXT, SANS, 0, "字符集覆盖"),
    ("ABC abc 0123 Il1| O0 ꞁ", 15, TEXT, SANS, 0, None),
    ("标点，。、；：？！“”‘’（）《》—…", 15, TEXT, SANS, 0, None),
    ("符号 →←↑↓ ✓✗ ●○■□ ★☆ §¶†‡ °±×÷ ≠≤≥ ∞≈", 15, TEXT, SANS, 0, None),
    ("路径 src/raster/renderer.cpp:128", 15, TEXT, SANS, 0, None),
    ("全角ＡＢＣ１２３　全角空格", 15, TEXT, SANS, 0, None),
    ("font_xs 12 · 辅助信息 · 版本号 v0.1.0", 12, TEXT, SANS, 0, "字号阶梯"),
    ("font_sm 13 · 次要说明文字", 13, TEXT, SANS, 0, None),
    ("font_base 15 · 正文默认字号", 15, TEXT, SANS, 0, None),
    ("font_lg 17 · 大号正文", 17, TEXT, SANS, 0, None),
    ("font_xl 20 · 小节标题", 20, TEXT, SANS, 0, None),
    ("text #0F172A 正文", 15, TEXT, SANS, 0, "颜色 token"),
    ("text_muted #56647C 次要", 15, MUTED, SANS, 0, None),
    ("text_faint #66768C 辅助", 15, FAINT, SANS, 0, None),
    ("primary #2563EB 主色", 15, PRIMARY, SANS, 0, None),
    ("常规 霜天组件画廊 Regular 15px", 15, TEXT, SANS, 0, "字重"),
    ("粗体 霜天组件画廊 Bold 15px", 15, TEXT, SANS, 700, None),
    ("半粗 霜天组件画廊 SemiBold 15px", 15, TEXT, SANS, 600, None),
    ("const auto polylines = p", 13.5, TEXT, MONO, 0, "等宽（代码）"),
    ("namespace st::raster { return a->b; }", 13.5, TEXT, MONO, 0, None),
    ("0123456789 => (){}[];: iIl1|0O", 13.5, TEXT, MONO, 0, None),
    ("中文注释必须能显示", 13.5, TEXT, MONO, 0, None),
    ("11px 最小字号 · 菜单项 · 状态栏", 11, TEXT, SANS, 0, "小字号压力"),
    ("12px 小字 · 混合 m4 S2 W3 · 日月水火", 12, TEXT, SANS, 0, None),
    ("13px 小字 · iliI1l 边界 · 明暗ab", 13, TEXT, SANS, 0, None),
    ("AVATAR To Wa fi fl ffi 连字测试", 15, TEXT, SANS, 0, "连字与行距"),
    ("行高测试第一行：重影是否出现", 15, TEXT, SANS, 0, None),
    ("行高测试第二行：重影是否出现", 15, TEXT, SANS, 0, None),
]

LINE_PITCH = 36.0     # 逻辑 px；> 最大字号 20 × 1.5 = 30，保证行带能被空白分开
LEFT = 4.0
TOP0 = 4.0
WIDTH = 700.0
HEIGHT = TOP0 + LINE_PITCH * len(ROWS) + 12.0


def cpp_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def cpp_color(hex_color: str) -> str:
    r, g, b = (int(hex_color[i:i + 2], 16) for i in (1, 3, 5))
    return f"Color{{0x{r:02X}, 0x{g:02X}, 0x{b:02X}, 0xFF}}"


def gen_html() -> str:
    out = [f"""<!doctype html>
<!-- 本文件由 `tools/gen_text_ab.py` 生成——**不要手改**（改了会在下次生成时丢失，
     而且两侧会漂移，漂移会被误读成"渲染差异"）。
     配套：`tools/text_ab_page_probe.cpp`（含同一张行表）与 `tools/text_ab_diff.py`。 -->
<meta charset="utf-8">
<style>
  html, body {{ margin: 0; padding: 0; background: #FFFFFF; }}
  /* 高度必须显式给：子元素全是 `position:absolute`、不撑开父容器，
     而 `overflow:hidden` 会按父容器**实际盒子**裁剪——body 高度 0 时整页被裁空
     （实测：截图全白、DOM 却完好）。 */
  body {{ position: relative; width: {WIDTH:.0f}px; height: {HEIGHT:.0f}px; overflow: hidden; }}
  .t {{ position: absolute; left: {LEFT:.0f}px; white-space: nowrap; line-height: 1; }}
</style>"""]
    for i, (text, size, color, family, weight, _group) in enumerate(ROWS):
        top = TOP0 + i * LINE_PITCH
        style = (f"top:{top:g}px; font-size:{size:g}px; color:{color}; "
                 f"font-family:{family}")
        if weight:
            style += f"; font-weight:{weight}"
        out.append(f'<div class="t" id="r{i:02d}" style="{style}">{text}</div>')
    return "\n".join(out) + "\n"


def cf(v: float) -> str:
    """C++ float 字面量：**总是带小数点**（`f"{v:g}f"` 会产出 `976f`，不是合法字面量）。"""
    s = f"{v:g}"
    if "." not in s and "e" not in s:
        s += ".0"
    return s + "f"


def gen_rows_inc() -> str:
    """C++ 行表（被 `text_ab_page_probe.cpp` #include）。"""
    out = [f"""// 本文件由 `tools/gen_text_ab.py` 生成——**不要手改**（见该脚本说明）。
// 行表与 `tools/text_ab_page.html` **同源**：改行内容请改生成器再重跑。
// 行距 {LINE_PITCH:g} 逻辑 px（> 最大字号 20 × 1.5，保证两侧的行带都能被空白分开）。
constexpr int kWidth = {WIDTH:.0f};
constexpr int kHeight = {HEIGHT:.0f};

struct Line {{
  std::string_view text;
  float top;        ///< 逻辑 y（= HTML 的 `top`）
  float size;       ///< 逻辑 px
  Color color;
  FontRole role;
  bool bold;
}};

constexpr std::array<Line, {len(ROWS)}> kLines{{{{"""]
    for i, (text, size, color, family, weight, _group) in enumerate(ROWS):
        top = TOP0 + i * LINE_PITCH
        role = "FontRole::Monospace" if family == MONO else "FontRole::Proportional"
        bold = "true" if weight == 700 else "false"
        out.append(f'    {{"{cpp_escape(text)}", {cf(top)}, {cf(size)}, {cpp_color(color)},\n'
                   f'     {role}, {bold}}},')
    out.append("}};")
    return "\n".join(out) + "\n"


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent
    (root / "text_ab_page.html").write_text(gen_html(), encoding="utf-8", newline="\n")
    (root / "text_ab_page_rows.inc").write_text(gen_rows_inc(), encoding="utf-8", newline="\n")
    print(f"已生成 text_ab_page.html 与 text_ab_page_rows.inc（{len(ROWS)} 行，"
          f"画布 {WIDTH:.0f}×{HEIGHT:.0f} 逻辑 px，行距 {LINE_PITCH:g}）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
