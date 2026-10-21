#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 DirectWrite vs 霜天 像素级对标的行表（单一真源，三份同源产物）。

产物（build/dwab/ 数据 + tools/dwab/ 行表 .inc）：
  rows-light.tsv   —— dwrite_ref.exe 读（label\tsize\ttop\tmono\tbold\ttext，UTF-8）
  rows-light.inc   —— 霜天 dw_ab_probe.cpp 编译期行表
  rows-light.json  —— 对比器 dw_ab_diff.py 的元数据（颜色/画布/行类）
  rows-dark.*      —— 深色主题同文本反色

行表设计（每一行都是一组"能分离某个机制"的字形）：
  han-body    常规正文汉字（笔画方向覆盖全）
  han-dense   高密度字（横笔画极多，量 stem 对齐/墨积）
  han-simple  稀疏笔画字（一 二 三 ——量横笔画相位）
  han-thin    细笔画字（撇捺点，量细笔画保真）
  latin-body  经典对齐样本句
  latin-thin  细竖笔组（i l l u m ——量 stem 宽度一致性）
  digits      数字（霜天有数字类 gamma 分档，重点核对）
  mixed       中英数字混排（量 advance/间距/基线）
  mono        Consolas 符号行（等宽链）
  size-XX     9~24px 阶梯（量小字号行为差异）
"""
import json
import pathlib

OUT = pathlib.Path(__file__).resolve().parent.parent / "build" / "dwab"
INC = pathlib.Path(__file__).resolve().parent / "dwab"
OUT.mkdir(exist_ok=True)

LIGHT_BG, LIGHT_FG = "#FFFFFF", "#0F172A"
DARK_BG, DARK_FG = "#0F1115", "#E8EEF9"

ROWS = [
    # (label, text, size, mono, bold)
    ("han-body",   "永东国画不清风飞翔", 15, 0, 0),
    ("han-dense",  "疆馨密霸繁悬整靓霾", 15, 0, 0),
    ("han-simple", "一二十上下三人", 15, 0, 0),
    ("han-thin",   "纤纱细线红绿经终", 15, 0, 0),
    ("latin-body", "Hamburgefonstiv III", 15, 0, 0),
    ("latin-thin", "illumination minimal", 15, 0, 0),
    ("digits",     "0123456789 3.14 42", 15, 0, 0),
    ("mixed",      "霜天渲染器 vs DirectWrite 123", 15, 0, 0),
    ("mono",       "->=>(){}[]; if (x) {", 15, 1, 0),
    ("size-09",    "永文字Aa9", 9, 0, 0),
    ("size-10",    "永文字Aa10", 10, 0, 0),
    ("size-11",    "永文字Aa11", 11, 0, 0),
    ("size-12",    "永文字Aa12", 12, 0, 0),
    ("size-13",    "永文字Aa13", 13, 0, 0),
    ("size-14",    "永文字Aa14", 14, 0, 0),
    ("size-16",    "永文字Aa16", 16, 0, 0),
    ("size-18",    "永文字Aa18", 18, 0, 0),
    ("size-24",    "永文字Aa24", 24, 0, 0),
]

LOGICAL_W = 640


def build():
    top = 8.0
    out = []
    for label, text, size, mono, bold in ROWS:
        line_h = max(26.0, size * 1.7)
        out.append({
            "label": label, "text": text, "size": size,
            "top": round(top, 1), "mono": bool(mono), "bold": bool(bold),
            "class": ("mono" if mono else ("han" if any('\u4e00' <= c <= '\u9fff' for c in text)
                                            else ("mixed" if any('\u4e00' <= c <= '\u9fff' for c in text)
                                                  else "latin"))),
        })
        # mixed 行含汉字归 han-mixed 单列
        if label == "mixed":
            out[-1]["class"] = "mixed"
        top += line_h
    height = round(top + 8.0, 1)
    return out, height


def write_all(rows, height, theme):
    bg, fg = (LIGHT_BG, LIGHT_FG) if theme == "light" else (DARK_BG, DARK_FG)
    meta = {"theme": theme, "bg": bg, "fg": fg, "width": LOGICAL_W, "height": height,
            "scale": 1.5, "rows": rows}
    (OUT / f"rows-{theme}.json").write_text(
        json.dumps(meta, ensure_ascii=False, indent=1), encoding="utf-8")

    with (OUT / f"rows-{theme}.tsv").open("w", encoding="utf-8", newline="\n") as f:
        for r in rows:
            f.write(f"{r['label']}\t{r['size']}\t{r['top']}\t{int(r['mono'])}\t{int(r['bold'])}\t{r['text']}\n")

    inc = []
    inc.append(f"// 由 tools/gen_dw_ab.py 生成（theme={theme}）——不要手改。")
    def rgb8(hexs):
        return f"{{0x{hexs[1:3]}, 0x{hexs[3:5]}, 0x{hexs[5:7]}}}"
    inc.append("namespace dwab_" + theme + " {")
    inc.append("struct DwColor { unsigned char r, g, b; };")
    inc.append(f"inline constexpr DwColor kDwBg = {rgb8(bg)};")
    inc.append(f"inline constexpr DwColor kDwFg = {rgb8(fg)};")
    inc.append(f"inline constexpr float kDwWidth = {LOGICAL_W}.0f;")
    inc.append(f"inline constexpr float kDwHeight = {height:.1f}f;")
    inc.append("struct DwLine { const char* label; const char* text; float size; float top; bool mono; bool bold; };")
    inc.append("inline constexpr DwLine kDwLines[] = {")
    for r in rows:
        mono = "true" if r["mono"] else "false"
        bold = "true" if r["bold"] else "false"
        inc.append(f'  {{"{r["label"]}", "{r["text"]}", {r["size"]:.1f}f, {r["top"]:.1f}f, {mono}, {bold}}},')
    inc.append("};")
    inc.append("}  // namespace dwab_" + theme)
    (INC / f"rows-{theme}.inc").write_text("\n".join(inc) + "\n", encoding="utf-8")


for theme in ("light", "dark"):
    rows, h = build()
    write_all(rows, h, theme)
    print(f"{theme}: {len(rows)} 行，画布 {LOGICAL_W}x{h} 逻辑 → {int(LOGICAL_W*1.5)}x{int(h*1.5)} 物理")
print("数据:", OUT, "\n行表 .inc:", INC)