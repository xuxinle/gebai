#!/usr/bin/env python3
"""从**单一真源**生成文字 A/B 对照的两侧文件（HTML + 探针的 C++ 行表）。

## 为什么要有这个生成器

对照的成立前提是两侧**逐字、逐字号、逐颜色、逐位置、逐字体**完全一致。手工维护两份表必然漂移
（第一版就漂了：HTML 用旧字号、探针用另一套）——而漂移导致的差异会被读成"渲染差异"。

## 字体必须先对齐（否则量的是字体不是渲染器）

2026-10-06 实测教训：对照页原先写 `'Segoe UI','Microsoft YaHei'`——Windows 上两者同名可用，
**Linux 上两个名字都不存在**，浏览器按 fontconfig 回退到 `Liberation Serif` / `Noto Sans CJK **JP**`，
而霜天侧走自己的候选链（`DejaVuSans.ttf` + `NotoSansCJK-Regular.ttc` 的 **SC** face）。
于是"墨量比 0.80、平均显著差 40.5%"里**大部分是字体本身的差别**，
拿它去调渲染器就是又一次"参照选错"。

现在两侧显式指定**同一套族名**：
  正文  `'DejaVu Sans','Noto Sans CJK SC'`  （= 霜天 `font_candidates()` 的前两名）
  等宽  `'DejaVu Sans Mono','Noto Sans CJK SC'`
`tools/text_ab_shot.mjs` 会导出浏览器**逐行实际字体归属**，`text_ab_diff.py` 对其与霜天侧
实测字体链比对，不一致就报出来——量尺自带"参照是否同一套"的断言。

## 行距为什么必须够大

行带切分靠"空白行"分隔。行距如果只比字高略大，相邻行的上伸部/下伸部会连成一片
（实测 22px 行距 / 15px 字号 → 28 行只切出 12~17 带），后续的按序配对就全错位了。
本生成器统一用 `LINE_PITCH`，并保证它 > 最大字号 × 1.5。

用法：python tools/gen_text_ab.py
      输出 tools/text_ab_chars_page.html / _rows.inc 与 tools/text_ab_sizes_page.html / _rows.inc
"""
import json
import pathlib

# 前景色（主题 token 原值，src/ui/theme.cpp）
TEXT = "#0F172A"
MUTED = "#56647C"
FAINT = "#66768C"
PRIMARY = "#2563EB"
# 与霜天 FontStack 的**前两名**逐位同序（见文件头说明）
SANS = "'DejaVu Sans','Noto Sans CJK SC'"
MONO = "'DejaVu Sans Mono','Noto Sans CJK SC'"

LINE_PITCH_PAD = 1.45     # 行距 = 最大字号 × 本系数（必须 > 1 且留出上/下伸部余量）
LEFT = 4.0
TOP0 = 4.0
WIDTH = 900.0

# ── 页 1：字符集覆盖（字母数字全量 / 简单-复杂汉字 / 标点符号） ──────────────
# (文本, 字号, 前景色, 字体族, 粗体, 分组标题)
CHARS_ROWS = [
    ("中文标题：组件画廊·资源管理器", 15, TEXT, SANS, 0, "中文字形（简单→复杂）"),
    ("一二十丁厂七卜人入八九几儿了力乃刀又", 15, TEXT, SANS, 0, None),
    ("三上下个大天大地小口山日月水火木金土", 15, TEXT, SANS, 0, None),
    ("中文测试字体渲染清晰组件画廊", 15, TEXT, SANS, 0, None),
    ("霜天字体渲染粗细均匀锐利清晰", 15, TEXT, SANS, 0, None),
    ("魏蜀黍龘龗爨蠹灩鬱麤齉齾", 15, TEXT, SANS, 0, None),
    ("藏彝僰瓯鼍蠡饕餮麒麟麒麟", 15, TEXT, SANS, 0, None),
    ("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 15, TEXT, SANS, 0, "拉丁字母（全量大写）"),
    ("abcdefghijklmnopqrstuvwxyz", 15, TEXT, SANS, 0, "拉丁字母（全量小写）"),
    ("0123456789 !\"#$%&'()*+,-./:;<=>?@", 15, TEXT, SANS, 0, "数字与 ASCII 符号"),
    ("[]^_`{|}~ \\ / | 0123456789", 15, TEXT, SANS, 0, None),
    ("Il1| O0 oO 8B S5 2Z rn m cl dq", 15, TEXT, SANS, 0, "易混字形"),
    ("AVATAR To Wa fi fl ffi ffl", 15, TEXT, SANS, 0, "连字与组合"),
    ("标点，。、；：？！“”‘’（）《》—…", 15, TEXT, SANS, 0, "中文标点与全角"),
    ("全角ＡＢＣａｂｃ１２３　全角空格", 15, TEXT, SANS, 0, None),
    ("符号 →←↑↓ ✓✗ ●○■□ ★☆ §¶†‡ °±×÷ ≠≤≥ ∞≈", 15, TEXT, SANS, 0, "符号与几何"),
    ("路径 src/raster/renderer.cpp:128", 15, TEXT, SANS, 0, "混合与界面串"),
    ("中英混排 Mixed 中英 2026 版 v0.1.0", 15, TEXT, SANS, 0, None),
    ("text #0F172A 正文", 15, TEXT, SANS, 0, "颜色 token"),
    ("text_muted #56647C 次要", 15, MUTED, SANS, 0, None),
    ("text_faint #66768C 辅助", 15, FAINT, SANS, 0, None),
    ("primary #2563EB 主色", 15, PRIMARY, SANS, 0, None),
    ("常规 霜天组件画廊 Regular", 15, TEXT, SANS, 0, "字重"),
    ("粗体 霜天组件画廊 Bold", 15, TEXT, SANS, 700, None),
    ("const auto polylines = points[i];", 13.5, TEXT, MONO, 0, "等宽（代码）"),
    ("namespace st::raster { return a->b; }", 13.5, TEXT, MONO, 0, None),
    ("0123456789 => (){}[];: iIl1|0O", 13.5, TEXT, MONO, 0, None),
    ("中文注释必须能显示 汉字宽度", 13.5, TEXT, MONO, 0, None),
]

# ── 页 2：字号阶梯（同一串、同一族，只变字号） ──────────────────────────────
SIZE_SAMPLE = "霜天组件画廊 Overview 2026 ABC abc"
SIZE_ROWS = [
    ("10px 霜天组件画廊 Overview 2026 ABC abc", 10, TEXT, SANS, 0, "字号阶梯（同一串逐档）"),
    ("11px 霜天组件画廊 Overview 2026 ABC abc", 11, TEXT, SANS, 0, None),
    ("12px 霜天组件画廊 Overview 2026 ABC abc", 12, TEXT, SANS, 0, None),
    ("13px 霜天组件画廊 Overview 2026 ABC abc", 13, TEXT, SANS, 0, None),
    ("13.5px 霜天组件画廊 Overview 2026 ABC", 13.5, TEXT, SANS, 0, None),
    ("14px 霜天组件画廊 Overview 2026 ABC abc", 14, TEXT, SANS, 0, None),
    ("15px 霜天组件画廊 Overview 2026 ABC abc", 15, TEXT, SANS, 0, None),
    ("16px 霜天组件画廊 Overview 2026 ABC abc", 16, TEXT, SANS, 0, None),
    ("17px 霜天组件画廊 Overview 2026 ABC abc", 17, TEXT, SANS, 0, None),
    ("20px 霜天组件画廊 Overview 2026 ABC abc", 20, TEXT, SANS, 0, None),
    ("24px 霜天组件画廊 Overview 2026 ABC", 24, TEXT, SANS, 0, None),
    ("32px 霜天组件画廊 Overview", 32, TEXT, SANS, 0, None),
    ("48px 霜天组件", 48, TEXT, SANS, 0, None),
    ("", 13.5, TEXT, MONO, 0, "等宽字号阶梯"),
    ("13.5px const auto p = x->y;", 13.5, TEXT, MONO, 0, None),
    ("15px const auto p = x->y;", 15, TEXT, MONO, 0, None),
    ("17px const auto p = x->y;", 17, TEXT, MONO, 0, None),
    ("20px const auto p = x->y;", 20, TEXT, MONO, 0, None),
]


def text_class(text: str, family: str) -> str:
    """按**实际渲染的文本**分类，而不是按行的标签。

    ⚠ 为什么必须显式给（2026-10-06 实测踩到）：下游脚本原先按行的**标签文本**分类，
    而标签里常混着两种字（如「常规 霜天组件画廊 Regular」「text #0F172A 正文」）——
    于是那些**以拉丁/等宽为主**的行被算进了汉字类，把汉字类的结果拉高，
    得出"拉丁/等宽偏重 1.10~1.16"的**错误方向**（干净分族测量是三类都在 0.95 附近）。
    分类是数据，不能靠猜字符串。
    """
    if not text:
        return "blank"
    has_cjk = any('\u4e00' <= ch <= '\u9fff' or '\u3000' <= ch <= '\u303f'
                  or '\uff00' <= ch <= '\uffef' for ch in text)
    has_latin = any(ch.isascii() and (ch.isalnum()) for ch in text)
    if family == MONO:
        return "mono"
    if has_cjk and has_latin:
        return "mixed"
    if has_cjk:
        return "cjk"
    return "latin"


def cpp_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def cpp_color(hex_color: str) -> str:
    r, g, b = (int(hex_color[i:i + 2], 16) for i in (1, 3, 5))
    return f"st::math::Color{{0x{r:02X}, 0x{g:02X}, 0x{b:02X}, 0xFF}}"


def cf(v: float) -> str:
    """C++ float 字面量：**总是带小数点**（`f"{v:g}f"` 会产出 `976f`，不是合法字面量）。"""
    s = f"{v:g}"
    if "." not in s and "e" not in s:
        s += ".0"
    return s + "f"


def layout(rows):
    """算出画布尺寸与每行 top（两侧共用；字号大于行距时按该行放大行距）。"""
    biggest = max(r[1] for r in rows)
    pitch = biggest * LINE_PITCH_PAD
    tops, y = [], TOP0
    for _text, size, *_ in rows:
        tops.append(y)
        y += max(pitch, size * LINE_PITCH_PAD)
    return y + 12.0, tops


def gen_html(rows, tops, height, title: str) -> str:
    out = [f"""<!doctype html>
<!-- 本文件由 `tools/gen_text_ab.py` 生成——**不要手改**（改了会在下次生成时丢失，
     而且两侧会漂移，漂移会被误读成"渲染差异"）。
     字体族必须与霜天 `FontStack` 候选链对齐，否则量到的是字体差异而不是渲染差异。 -->
<meta charset="utf-8">
<title>{title}</title>
<style>
  html, body {{ margin: 0; padding: 0; background: #FFFFFF; }}
  /* 高度必须显式给：子元素全是 `position:absolute`、不撑开父容器，
     而 `overflow:hidden` 会按父容器**实际盒子**裁剪——body 高度 0 时整页被裁空
     （实测：截图全白、DOM 却完好）。 */
  body {{ position: relative; width: {WIDTH:.0f}px; height: {height:.0f}px; overflow: hidden; }}
  /* `text-rendering: geometricPrecision` 关掉浏览器的"整数像素推进"优化：
     我们要比的是**字形栅格化**，不是排版取整（取整会让字距在两侧各偏一半像素）。 */
  .t {{ position: absolute; left: {LEFT:.0f}px; white-space: nowrap; line-height: 1;
        text-rendering: geometricPrecision; }}
</style>"""]
    for i, (text, size, color, family, weight, _group) in enumerate(rows):
        top = tops[i]
        style = (f"top:{top:g}px; font-size:{size:g}px; color:{color}; "
                 f"font-family:{family}")
        if weight:
            style += f"; font-weight:{weight}"
        body = text if text else "&nbsp;"
        out.append(f'<div class="t" id="r{i:02d}" style="{style}">{body}</div>')
    return "\n".join(out) + "\n"


def gen_rows_inc(rows, tops, height, ns: str) -> str:
    out = [f"""// 本文件由 `tools/gen_text_ab.py` 生成——**不要手改**（见该脚本说明）。
// 行表与同名 HTML **同源**：改行内容请改生成器再重跑。
constexpr int kWidth = {WIDTH:.0f};
constexpr int kHeight = {height:.0f};

struct Line {{
  std::string_view text;
  float top;        ///< 逻辑 y（= HTML 的 `top`）
  float size;       ///< 逻辑 px
  st::math::Color color;
  st::text::FontRole role;
  bool bold;
}};

constexpr std::array<Line, {len(rows)}> kLines{{{{"""]
    for i, (text, size, color, family, weight, _group) in enumerate(rows):
        role = ("st::text::FontRole::Monospace" if family == MONO
                else "st::text::FontRole::Proportional")
        bold = "true" if weight == 700 else "false"
        out.append(f'    {{"{cpp_escape(text)}", {cf(tops[i])}, {cf(size)}, {cpp_color(color)},\n'
                   f'     {role}, {bold}}},')
    out.append("}};")
    return "\n".join(out) + "\n"


def emit(root: pathlib.Path, name: str, rows, title: str) -> None:
    height, tops = layout(rows)
    (root / f"text_ab_{name}_page.html").write_text(gen_html(rows, tops, height, title),
                                                    encoding="utf-8", newline="\n")
    (root / f"text_ab_{name}_rows.inc").write_text(gen_rows_inc(rows, tops, height, name),
                                                   encoding="utf-8", newline="\n")
    # 行元数据（JSON）：供 `text_ab_diff.py` / `text_ab_sheet.py` 读**同一张表**。
    # 它们原先各自硬编码行表，改一次字号/文本就要改三处——必然漂移，
    # 而漂移出来的错位会被读成"渲染差异"。
    meta = [{"row": i, "label": text if text else "（空行）", "size": size,
             "color": color, "mono": family == MONO, "bold": bool(weight),
             "class": text_class(text, family), "group": group, "top": tops[i]}
            for i, (text, size, color, family, weight, group) in enumerate(rows)]
    (root / f"text_ab_{name}_rows.json").write_text(
        json.dumps({"width": WIDTH, "height": height, "rows": meta},
                   ensure_ascii=False, indent=1), encoding="utf-8", newline="\n")
    print(f"已生成 text_ab_{name}_page.html / _rows.inc / _rows.json（{len(rows)} 行，"
          f"画布 {WIDTH:.0f}×{height:.0f} 逻辑 px）")


# ── 页 3：字族 × 字号（回答"差异是否随字族/字号变化"） ──────────────────────
# 为什么单独一页：字符集页把三个字族**混在同一段文字**里，测出来的差异无法归因；
# 而字号阶梯页每行都是中英混排，同样分不开。要判"fit 对真型字体是否过锐"，
# 必须让**同一字族在同一行里、只变字号**。
FAMILY_SIZES = [11, 13, 15, 20, 32]
FAMILY_TEXT = {
    "latin": ("Handgloves 0123 ABC abc", SANS),
    "cjk": ("霜天组件画廊渲染测试", SANS),
    "mono": ("const auto p = x;", MONO),
}


def family_rows():
    rows = []
    for size in FAMILY_SIZES:
        for key in ("latin", "cjk", "mono"):
            text, family = FAMILY_TEXT[key]
            rows.append((f"{text}", size, TEXT, family, 0,
                         f"{key} {size}px" if key != "mono" else None))
    return rows


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent
    emit(root, "chars", CHARS_ROWS, "霜天文字 A/B — 字符集")
    emit(root, "sizes", SIZE_ROWS, "霜天文字 A/B — 字号阶梯")
    emit(root, "families", family_rows(), "霜天文字 A/B — 字族 × 字号")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
