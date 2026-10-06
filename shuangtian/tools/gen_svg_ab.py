#!/usr/bin/env python3
"""生成**真实 SVG 矢量图形**的 A/B 页（亮/暗）与其霜天侧行表。

与 `gen_text_ab.py`（文字页）并列：那条线量的是**字形栅格化**，本条线量的是
**矢量光栅化**（路径填充 / 圆角 / 变换 / 描边 / 填充规则）——没有字体参与，
所以差异能被干净归因到光栅器本身。

每个 case 用**同一份 SVG 源码**同时喂给两侧（浏览器 `<img>` 或内联 `<svg>`、
霜天 `svg::parse` + `svg::draw`），生成时把源码与几何参数写进 JSON 供探针读。
"""

import json
import pathlib

W = 900
CASES = []


def case(name, svg_body, size=64, view_box=None):
    """登记一个 case：`svg_body` 是 `<svg>` 的内容（不带根标签）。"""
    CASES.append({"name": name, "body": svg_body, "size": size, "view_box": view_box})


# ① 路径填充：曲线 + 闭合 + 自交（evenodd/nonzero 的差异敏感区）
case("path-curve", '<path d="M8 56 C 8 24, 24 8, 32 8 C 40 8, 56 24, 56 56 Z" '
                   'fill="#2563EB"/>')
case("path-evenodd", '<path d="M4 4 H60 V60 H4 Z M16 16 H48 V48 H16 Z" fill="#DC2626" '
                     'fill-rule="evenodd"/>')
case("path-nonzero", '<path d="M4 4 H60 V60 H4 Z M16 16 H48 V48 H16 Z" fill="#DC2626"/>')
# ② 圆弧指令（A 指令端点→圆心参数化，最易出错）
case("path-arc", '<path d="M8 32 A 24 24 0 0 1 56 32" fill="none" stroke="#059669" '
                 'stroke-width="5"/>')
# ③ 基本形状：圆角矩形（rx/ry）+ 圆 + 椭圆
case("rect-round", '<rect x="6" y="12" width="52" height="40" rx="12" ry="8" fill="#7C3AED"/>')
case("circle", '<circle cx="32" cy="32" r="26" fill="#EA580C"/>')
case("ellipse", '<ellipse cx="32" cy="32" rx="28" ry="16" fill="#0891B2"/>')
# ④ 描边：线帽 / 连接 / 虚线
case("polyline", '<polyline points="6,50 20,18 34,42 48,12 58,30" fill="none" '
                 'stroke="#2563EB" stroke-width="4" stroke-linecap="round" '
                 'stroke-linejoin="round"/>')
case("polygon", '<polygon points="32,6 58,52 6,52" fill="#16A34A"/>')
# ⑤ 变换：rotate / scale / translate / matrix
case("transform-rotate", '<rect x="20" y="20" width="24" height="24" fill="#DB2777" '
                         'transform="rotate(30 32 32)"/>')
case("transform-matrix", '<rect x="14" y="22" width="36" height="20" fill="#4338CA" '
                         'transform="matrix(0.9 0.3 -0.3 0.9 8 -4)"/>')
# ⑥ 级联与继承：`<g>` 上的 fill + 子元素覆盖
case("group-cascade", '<g fill="#059669"><rect x="6" y="20" width="20" height="24"/>'
                      '<rect x="36" y="20" width="20" height="24" fill="#DC2626"/></g>')
# ⑦ 不透明叠加（覆盖率的**顺序/合成**语义）
case("opacity-stack", '<rect x="8" y="8" width="40" height="40" fill="#2563EB" '
                      'fill-opacity="0.5"/>'
                      '<rect x="24" y="24" width="40" height="40" fill="#DC2626" '
                      'fill-opacity="0.5"/>')
# ⑧ 细线（光栅化的**最敏感区**：1px 线在分数坐标上）
case("hairline", '<line x1="4" y1="32.5" x2="60" y2="32.5" stroke="#111" stroke-width="1"/>')
case("hairline-frac", '<line x1="4" y1="32.4" x2="60" y2="32.4" stroke="#111" stroke-width="1"/>')
# ⑨ 斜细线（抗锯齿质量）
case("diagonal-thin", '<line x1="6" y1="56" x2="58" y2="8" stroke="#111" stroke-width="1.5"/>')
# ⑩ 当前色继承（图标库常用）
case("current-color", '<g stroke="currentColor" fill="none" stroke-width="4">'
                      '<circle cx="32" cy="32" r="22"/></g>')

ROOT_ATTRS = 'xmlns="http://www.w3.org/2000/svg" fill="none" stroke="currentColor" ' \
             'stroke-width="2" stroke-linecap="round" stroke-linejoin="round"'


def svg_source(c, color_override=None):
    vb = c["view_box"] or f"0 0 64 64"
    body = c["body"]
    if color_override and "currentColor" in body:
        body = body.replace("currentColor", color_override)
    return (f'<svg {ROOT_ATTRS} viewBox="{vb}" width="{c["size"]}" height="{c["size"]}">'
            f'{body}</svg>')


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent
    COLS = 6
    CELLSIZE = 96
    rows = (len(CASES) + COLS - 1) // COLS
    total_h = rows * CELLSIZE
    for theme, bg in (("light", "#FFFFFF"), ("dark", "#0F1115")):
        sfx = "" if theme == "light" else "_dark"
        fg = "#111111" if theme == "light" else "#E6EAF2"
        # 网格布局：每格 `CELLSIZE`×`CELLSIZE`，图形居中放其中（两侧**同格**，
        # 于是逐格比较时坐标天然对齐——这是让差异可归因的前提）。
        parts = []
        for idx, c in enumerate(CASES):
            off = (CELLSIZE - c["size"]) // 2
            # ⚠ 画布尺寸必须是**图形尺寸**（`c["size"]`），不能是格子尺寸——
            # 用格子尺寸会把内容放大 `CELLSIZE/size` 倍，面积差 (96/64)²≈2.3 倍
            # （实测：所有 case 的墨量比整齐地停在 0.43，正是这个比例）。
            parts.append(
                f'<svg width="{c["size"]}" height="{c["size"]}" '
                f'style="position:absolute;left:{(idx % COLS) * CELLSIZE + off}px;'
                f'top:{(idx // COLS) * CELLSIZE + off}px" '
                f'{ROOT_ATTRS} viewBox="{c["view_box"] or "0 0 64 64"}">'
                f'{c["body"].replace("currentColor", fg)}</svg>')
        page = (
            '<!doctype html>\n<meta charset="utf-8">\n<title>svg-ab</title>\n<style>\n'
            f'  html, body {{ margin: 0; padding: 0; background: {bg}; }}\n'
            f'  body {{ position: relative; width: {COLS * CELLSIZE}px; '
            f'height: {total_h}px; font-size: 0; }}\n'
            '  .cap { font: 11px sans-serif; color: #888; height: 14px; line-height: 14px; }\n'
            '  svg { display: block; }\n'
            '</style>\n<body>\n' + "\n".join(parts) + "\n</body>\n")
        (root / f"svg_ab_page{sfx}.html").write_text(page, encoding="utf-8", newline="\n")
        meta = {"cols": COLS, "cell": CELLSIZE, "rows": rows,
                "cases": [{"name": c["name"], "size": c["size"], "idx": i,
                           "view_box": c["view_box"] or "0 0 64 64",
                           "source": svg_source(c, fg)} for i, c in enumerate(CASES)]}
        (root / f"svg_ab_cases{sfx}.json").write_text(
            json.dumps(meta, ensure_ascii=False, indent=1), encoding="utf-8", newline="\n")
        print(f"已生成 svg_ab_page{sfx}.html / svg_ab_cases{sfx}.json（{len(CASES)} case）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
