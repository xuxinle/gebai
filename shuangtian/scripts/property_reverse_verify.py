#!/usr/bin/env python3
"""逆向验证：逐条回退属性描述/面板生成的修复，确认对应用例真的变红。

## 为什么必须做

一条永远绿的测试只是代码。本仓实测过三种"假绿"，本文件第 ③ 类最凶：
**测试把公式又抄了一遍**——实现写错、抄件照着错，测试照样绿。
面板映射的第一版就是这样（自己在测试里算 min + span*unit）。
现在用例一律调 `property_from_unit` 真实实现，所以回退它一定能被抓住。

## 用法

    python scripts/property_reverse_verify.py [--only 关键词] [--list]
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCHEMA_CPP = ROOT / "src" / "ui" / "property_schema.cpp"
CAIYUN_SCHEMA = ROOT / "examples" / "caiyun" / "layer_schema.cpp"
ST_BIN = ROOT / "build" / "bin" / "st.exe"

CASES = [
    {
        "name": "单位换算把区间当成 0..span（中点偏移）",
        "file": SCHEMA_CPP,
        "original": "  const double raw = range.min + span * std::clamp(unit, 0.0, 1.0);",
        "broken": "  const double raw = span * std::clamp(unit, 0.0, 1.0);",
        "expect_fail": ["property_panel_range_map_is_symmetric"],
    },
    {
        "name": "整数不落格（显示 7 实际 7.3）",
        "file": SCHEMA_CPP,
        "original": "  if (desc.type == PropertyType::Integer) value = std::round(value);",
        "broken": "  // 回退：不落格",
        "expect_fail": ["property_panel_integer_snaps_to_whole"],
    },
    {
        "name": "step 吸附被去掉（值不落在网格上）",
        "file": SCHEMA_CPP,
        "original": """    if (desc.step > 0.0f) {
      const double origin = static_cast<double>(desc.min_value);
      const double offset = (value - origin) / static_cast<double>(desc.step);
      value = origin + std::round(offset) * static_cast<double>(desc.step);""",
        "broken": """    if (false) {
      const double origin = static_cast<double>(desc.min_value);
      const double offset = (value - origin) / static_cast<double>(desc.step);
      value = origin + std::round(offset) * static_cast<double>(desc.step);""",
        "expect_fail": ["property_panel_step_is_respected_across_the_whole_range"],
    },
    {
        "name": "夹取上界丢失（越界值透出去）",
        "file": SCHEMA_CPP,
        "original": """    value = std::clamp(value, static_cast<double>(desc.min_value),
                       static_cast<double>(desc.max_value));""",
        "broken": """    value = std::max(value, static_cast<double>(desc.min_value));""",
        "expect_fail": ["property_panel_clamps_outside_unit_interval"],
    },
    {
        "name": "图层表的夹取不来自描述（另写一套上界）",
        "file": CAIYUN_SCHEMA,
        "original": "  const auto desc = st::ui::find_property_desc(kLayerProperties, name);\n  if (desc == nullptr) return false;",
        "broken": "  const auto desc = st::ui::find_property_desc(kLayerProperties, name);\n  if (desc == nullptr) return false;\n  // 回退：写入路径不再用描述的范围\n  (void)desc;",
        "expect_fail": ["layer_schema_every_name_is_readable_and_writable"],
    },
    {
        "name": "图层表登记了不存在的属性名",
        "file": CAIYUN_SCHEMA,
        "original": '    {.name = "opacity", .type = PropertyType::Number, .min_value = 0.0f, .max_value = 1.0f,',
        "broken": '    {.name = "opacity_typo", .type = PropertyType::Number, .min_value = 0.0f, .max_value = 1.0f,',
        "expect_fail": ["layer_schema_every_name_is_readable_and_writable"],
    },
    {
        "name": "颜色解析不再接受三位简写",
        "file": CAIYUN_SCHEMA,
        "original": "  if (digits == 3) {",
        "broken": "  if (digits == 3 && false) {",
        "expect_fail": ["layer_schema_color_round_trips"],
    },
    {
        "name": "浅底判据被关掉（米白片又长出黑角）",
        "file": ROOT / "examples" / "caiyun" / "fx.cpp",
        "original": "  return std::max(luma(top), luma(bottom)) >= kLightThreshold;",
        "broken": "  return false;   // 回退：永远当成深底",
        "expect_fail": ["backdrop_light_detects_cream_and_white"],
    },
    {
        "name": "只看向 bg_top（一亮一暗的渐变漏掉）",
        "file": ROOT / "examples" / "caiyun" / "fx.cpp",
        "original": "  return std::max(luma(top), luma(bottom)) >= kLightThreshold;",
        "broken": "  return luma(top) >= kLightThreshold;   // 回退：只看上面那侧",
        "expect_fail": ["backdrop_light_uses_the_brighter_side"],
    },
    {
        "name": "误用 WCAG 线性亮度（中浅米色被判成深色）",
        "file": ROOT / "examples" / "caiyun" / "fx.cpp",
        "original": "  return std::max(luma(top), luma(bottom)) >= kLightThreshold;",
        "broken": "  return std::max(st::math::relative_luminance(top), st::math::relative_luminance(bottom)) >= kLightThreshold;",
        "expect_fail": ["backdrop_light_uses_the_brighter_side"],
    },
    {
        "name": "源侧销点漏算（渲染时销点随倍率漂走）",
        "file": ROOT / "examples" / "caiyun" / "fx.cpp",
        "original": "      move.anchor_x >= 0.0f ? move.anchor_x * src_w - 0.5f : src_w * 0.5f;",
        "broken": "      src_w * 0.5f;   // 回退：源侧仍绕画面中心",
        "expect_fail": ["zoom_anchor_stays_put_at_every_scale"],
    },
    {
        "name": "销点忘了连续的→像素索引转换（半像素被放大）",
        "file": ROOT / "examples" / "caiyun" / "fx.cpp",
        "original": "      move.anchor_x >= 0.0f ? move.anchor_x * src_w - 0.5f : src_w * 0.5f;",
        "broken": "      move.anchor_x >= 0.0f ? move.anchor_x * src_w : src_w * 0.5f;",
        "expect_fail": ["zoom_anchor_stays_put_at_every_scale"],
    },
    {
        "name": "指数曲线的归一化丢了（两端落不到 0/1）",
        "file": ROOT / "examples" / "caiyun" / "fx.cpp",
        "original": "    t_zoom = raw / (1.0f - 1.0f / kBase);",
        "broken": "    t_zoom = t;   // 回退：退化成线性",
        "expect_fail": ["zoom_expo_in_is_mostly_late"],
    },
    {
        "name": "相机窗口失效（窗口后仍推进，钻到位停不住）",
        "file": ROOT / "examples" / "caiyun" / "fx.cpp",
        "original": "  return std::clamp((time - window_start) / std::max(0.05f, span), 0.0f, 1.0f);",
        "broken": "  (void)window_start; (void)span; return std::clamp(time / std::max(0.2f, window_end), 0.0f, 1.0f);",
        "expect_fail": ["fx_camera_window_clamps_outside_and_advances_inside"],
    },
    {
        "name": "矢量相机锚点算错（1x 对、大倍率漂走）",
        "file": ROOT / "examples" / "caiyun" / "fx.hpp",
        "original": "    return {(x - anchor_x) * zoom + anchor_x, (y - anchor_y) * zoom + anchor_y};",
        "broken": "    return {x * zoom, y * zoom};   // 回退：绕原点缩放（锚点无效）",
        "expect_fail": ["vector_camera_anchor_stays_put"],
    },
    {
        "name": "矢量相机 zoom=1 不再恒等",
        "file": ROOT / "examples" / "caiyun" / "fx.hpp",
        "original": "    if (zoom == 1.0f) return {x, y};",
        "broken": "    // 回退：去掉早退（zoom=1 时多算一遍，浮点误差）",
        "expect_fail": ["vector_camera_identity_when_zoom_is_one"],
    },
    {
        "name": "物理像素入口的 scale 除错了（path/ellipse 不过相机）",
        "file": ROOT / "examples" / "caiyun" / "fx.hpp",
        "original": "    if (zoom == 1.0f) return {px, py};\n    const auto [x, y] = apply(px / scale, py / scale);",
        "broken": "    if (zoom == 1.0f) return {px, py};\n    const auto [x, y] = apply(px, py);   // 回退：忘了除 scale",
        "expect_fail": ["vector_camera_physical_entry_matches_design_entry"],
    },
]


def run_test(name: str) -> bool:
    result = subprocess.run(
        [str(ST_BIN), "test", name, "--profile", "dev"],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    output = (result.stdout or "") + (result.stderr or "")
    return f"PASS {name}" in output


def verify(case: dict) -> tuple[bool, str]:
    target: pathlib.Path = case["file"]
    text = target.read_text(encoding="utf-8")
    original = case["original"]
    if text.count(original) != 1:
        return False, f"原文出现 {text.count(original)} 次（需恰好 1 次）"
    broken = text.replace(original, case["broken"], 1)
    try:
        # 删旧文件再写：`copy2` 会把旧 mtime 搬回来，增量构建认为源没变
        # → 跑的是旧二进制 → 假绿。
        target.unlink()
        target.write_text(broken, encoding="utf-8")
        green = [n for n in case["expect_fail"] if run_test(n)]
        if green:
            return False, f"回退后仍绿（护栏无效）：{green}"
        return True, f"回退后 {len(case['expect_fail'])} 条全部变红"
    finally:
        target.unlink()
        target.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--only", default=None)
    args = parser.parse_args()

    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="replace")

    cases = [c for c in CASES if not args.only or args.only in c["name"]]
    if args.list:
        for index, case in enumerate(cases, 1):
            print(f"{index}. {case['name']} → {', '.join(case['expect_fail'])}")
        return 0
    if not ST_BIN.exists():
        print(f"找不到 {ST_BIN}（先跑一次 st test）", file=sys.stderr)
        return 2

    failures = 0
    for index, case in enumerate(cases, 1):
        print(f"[{index}/{len(cases)}] {case['name']} … ", end="", flush=True)
        ok, detail = verify(case)
        print(detail)
        if not ok:
            failures += 1
    print()
    if failures:
        print(f"{failures}/{len(cases)} 条回退没让测试变红——护栏不可信", file=sys.stderr)
        return 1
    print(f"全部 {len(cases)} 条回退都让测试变红：护栏可复验")
    return 0


if __name__ == "__main__":
    sys.exit(main())
