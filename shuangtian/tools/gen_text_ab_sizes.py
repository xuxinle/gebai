#!/usr/bin/env python3
"""生成**字号阶梯**对照页（同一段字、只变字号）——用于推导「字号 → gamma」映射。

## 为什么不复用 `gen_text_ab.py` 那张表

那张表的每一行**文本内容不同**（中文/英文/符号/等宽…），所以逐行的"墨量比"里混着
**内容差异**——实测同一批数据推出的理想 γ 完全不单调（18px→1.40、22.5px→0.87），
那是噪声不是趋势，拿它拟合曲线就是**过拟合噪声**。

本页只做一件事：**同一串字，只改字号**。这样逐行墨量比的差异就只能来自字号本身。

用法：python tools/gen_text_ab_sizes.py
      输出 tools/text_ab_sizes.html 与 tools/text_ab_sizes_rows.inc
"""

import pathlib

# 字串覆盖中英数（三种笔画特征），且长度适中（够多笔画、又不会撑破画布）
SAMPLE = "组件画廊 Overview 24"
SIZES = [10, 11, 12, 13, 14, 15, 16, 18, 20, 24, 28, 32]
LINE_PITCH = 56.0     # > 最大字号 32 × 1.5 = 48，保证行带能被空白分开
LEFT, TOP0, WIDTH = 4.0, 4.0, 560.0
HEIGHT = TOP0 + LINE_PITCH * len(SIZES) + 16.0
COLOR = "#0F172A"
FAMILY = "'Segoe UI','Microsoft YaHei'"


def gen_html() -> str:
    out = [f"""<!doctype html>
<!-- 由 `tools/gen_text_ab_sizes.py` 生成——**不要手改**。
     用途：同一段字、只变字号的对照页，用于推导「字号 → gamma」映射。 -->
<meta charset="utf-8">
<style>
  html, body {{ margin: 0; padding: 0; background: #FFFFFF; }}
  body {{ position: relative; width: {WIDTH:.0f}px; height: {HEIGHT:.0f}px; overflow: hidden; }}
  .t {{ position: absolute; left: {LEFT:.0f}px; white-space: nowrap; line-height: 1; }}
</style>"""]
    for i, size in enumerate(SIZES):
        top = TOP0 + i * LINE_PITCH
        out.append(f'<div class="t" id="s{i:02d}" '
                   f'style="top:{top:g}px; font-size:{size:g}px; color:{COLOR}; '
                   f'font-family:{FAMILY}">{SAMPLE}</div>')
    return "\n".join(out) + "\n"


def cf(v: float) -> str:
    """C++ float 字面量：**总是带小数点**（`f"{v:g}f"` 会产出 `4f`，不是合法字面量）。"""
    s = f"{v:g}"
    if "." not in s and "e" not in s:
        s += ".0"
    return s + "f"


def gen_rows_inc() -> str:
    out = [f"""// 由 `tools/gen_text_ab_sizes.py` 生成——**不要手改**。
// 同一段字、只变字号；行距 {LINE_PITCH:g} 逻辑 px（> 最大字号 32 × 1.5）。
constexpr int kWidth = {WIDTH:.0f};
constexpr int kHeight = {HEIGHT:.0f};
constexpr char kSample[] = "{SAMPLE}";

struct Line {{
  float top;
  float size;
}};

constexpr std::array<Line, {len(SIZES)}> kLines{{{{"""]
    for i, size in enumerate(SIZES):
        top = TOP0 + i * LINE_PITCH
        out.append(f"    {{{cf(top)}, {cf(size)}}},")
    out.append("}};")
    return "\n".join(out) + "\n"


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent
    (root / "text_ab_sizes.html").write_text(gen_html(), encoding="utf-8", newline="\n")
    (root / "text_ab_sizes_rows.inc").write_text(gen_rows_inc(), encoding="utf-8", newline="\n")
    print(f"已生成 text_ab_sizes.html 与 .inc（{len(SIZES)} 档字号：{SIZES}，"
          f"画布 {WIDTH:.0f}×{HEIGHT:.0f}）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
