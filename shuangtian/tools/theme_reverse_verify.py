#!/usr/bin/env python3
"""逆向验证：逐条把改动临时回退，确认对应回归用例**真的变红**。

为什么必须做：一个"恒绿"的测试看起来像护栏，实际什么都拦不住。
本脚本对每条关键契约做一次「回退 → 期望红 → 恢复」。

用法：python3 tools/theme_reverse_verify.py
"""
import os
import re
import shutil
import subprocess
import sys

THEME_CPP = "src/ui/theme.cpp"
THEME_IO_CPP = "src/ui/theme_io.cpp"
ELEMENT_CPP = "src/ui/element.cpp"
TEXT_PORT_HPP = "include/st/ui/text_port.hpp"
SERVER_CPP = "src/control/server.cpp"
PATH_CPP = "src/raster/path.cpp"
CANVAS_CPP = "src/raster/canvas.cpp"
RASTERIZER_CPP = "src/raster/rasterizer.cpp"

# (说明, 文件, 原文, 回退后的文本, 期望变红的用例名)
CASES = [
    (
        "亮/暗表面阶梯：把暗色 surface 还原成旧值（与 bg 仅差 1.054）",
        THEME_CPP,
        "palette.surface = hex(0x15151AFFU);",
        "palette.surface = hex(0x101014FFU);",
        "theme_surface_ladder_is_monotone_and_distinguishable",
    ),
    (
        "凹槽语义：把亮色 surface_sunken 还原成比 bg 只暗一点的值",
        THEME_CPP,
        "palette.surface_sunken = hex(0xDDDDE4FFU);",
        "palette.surface_sunken = hex(0xE0E0E6FFU);",
        "theme_surface_ladder_is_monotone_and_distinguishable",
    ),
    (
        "浅色阴影浓度：把基色 alpha 还原成会超 16% 上限的值",
        THEME_CPP,
        "palette.shadow = hex(0x0A0A1226U);",
        "palette.shadow = hex(0x0A0A1233U);",
        "theme_shadow_tiers_are_layered_and_ordered",
    ),
    (
        "主题名：去掉 dark() 的 name_ 赋值",
        THEME_CPP,
        'theme.name_ = "dark";\n',
        "",
        "theme_name_survives_construction",
    ),
    (
        "玻璃边缘：把亮色的 highlight 改成不透明白（浅底上不可见却被断言为\"应透明\"）",
        THEME_CPP,
        "palette.highlight = hex(0xFFFFFF00U);",
        "palette.highlight = hex(0xFFFFFFFFU);",
        "theme_glass_edge_is_visible_only_where_it_helps",
    ),
    (
        "未知 token：让拼错的键被静默忽略（而不是报错）",
        THEME_IO_CPP,
        '    return unexpected(ErrorCode::Invalid, std::format("未知主题 token：{}", joined));',
        "    return {};  // 逆向验证：静默忽略",
        "theme_json_unknown_token_is_reported_not_ignored",
    ),
    (
        "alpha 小数语义：把 `0.5` 当成通道值 0.5（而不是 0..1 比例）",
        THEME_IO_CPP,
        "  if (is_fraction) value *= 255.0;",
        # 注意：回退文本必须**能编译过**。写成注释会让 `is_fraction` 变成
        # set-but-unused，-Werror 把编译打断，测试实际跑的是上一份二进制
        # —— 于是得到一条“假绿”（这个坑本脚本自己踩过一次）。
        "  if (is_fraction) value *= 1.0;",
        "theme_json_accepts_documented_color_syntaxes",
    ),
    (
        "悬浮特效：把背景改回“直接 mix surface_hover”（hover_t=1 时完全替换）",
        # 回退文本必须**能编译过**（上次写成注释掉了赋值，-Werror 打断编译，
        # 测试跑的是旧二进制 → 假绿）。这里保留引用以免未使用告警。
        ELEMENT_CPP,
        """    if (background.a == 0U) {
      // 无底（Ghost）平铺一层半透明中性色：它的"语气"本就不在底色上。
      background = math::Color{colors.surface_hover.r, colors.surface_hover.g,
                               colors.surface_hover.b,
                               static_cast<std::uint8_t>(colors.surface_hover.a * hover_t)};
    } else {
      const bool dark_base = static_cast<float>(background.r) < 128.0f;
      const float delta = dark_base ? up : -down;""",
        """    if (background.a == 0U) {
      background = math::Color{colors.surface_hover.r, colors.surface_hover.g,
                               colors.surface_hover.b,
                               static_cast<std::uint8_t>(colors.surface_hover.a * hover_t)};
    } else if (true) {
      // 逆向验证：回退成旧的"直接混合"（hover_t=1 时完全替换）
      background = background.mix(colors.surface_hover, hover_t);
      (void)up; (void)down;
    } else {
      const bool dark_base = static_cast<float>(background.r) < 128.0f;
      const float delta = dark_base ? up : -down;""",
        "hover_keeps_the_surface_tone_instead_of_flattening_it",
    ),
    (
        "文字垂直居中：退回“按行盒居中”（不看墨迹）",
        TEXT_PORT_HPP,
        """  const auto ink = port.ink_metrics(text, size);
  if (!ink) return box_centered;""",
        """  // 逆向验证：退回按行盒居中
  (void)text;
  return box_centered;
  const auto ink = port.ink_metrics(text, size);
  if (!ink) return box_centered;""",
        "text_vertical_center_aligns_ink_not_line_box",
    ),
    (
        "文字垂直居中：用主面 `ascent` 代替排版的真实基线位",
        TEXT_PORT_HPP,
        "      port.shaped_ascent(text, size) - (ink->above + ink->below) * 0.5f;",
        "      port.ascent(size) - (ink->above + ink->below) * 0.5f;",
        "text_vertical_center_uses_the_shaped_baseline",
    ),
    (
        "卡片边框：取消像素对齐（小数坐标直接画环，1px 边被摊到两行）",
        ELEMENT_CPP,
        """    const math::Rect snapped{
        std::round(box.x), std::round(box.y),
        std::max(0.0f, std::round(box.right()) - std::round(box.x)),
        std::max(0.0f, std::round(box.bottom()) - std::round(box.y))};""",
        "    const math::Rect snapped = box;",
        "border_edge_lands_on_whole_pixels_after_snapping",
    ),
    (
        "capture 区域：把扁平参数改回“静默忽略”（而不是报错）",
        SERVER_CPP,
        """      if (has_flat) {
        return unexpected(ErrorCode::Invalid,
                          "区域参数必须包在 region 对象里，如 {region:{x,y,width,height}}"
                          "（扁平的 x/y/width/height 会被忽略并静默截全屏）");
      }""",
        """      (void)has_flat;   // 逆向验证：静默忽略扁平参数""",
        "capture_region_rejects_half_formed_params",
    ),
    (
        "capture 尺寸：把 pixel_size 改回“拿 region 算”（夹取时谎报）",
        SERVER_CPP,
        """  Json pixels = Json::object();
  pixels["width"] = static_cast<std::int64_t>(actual_width);
  pixels["height"] = static_cast<std::int64_t>(actual_height);""",
        """  Json pixels = Json::object();
  {
    const double reverse_scale = static_cast<double>(host.device_scale());
    const math::Size reverse_vp = host.viewport();
    const int rw = region.is_empty() ? static_cast<int>(reverse_vp.width) : region.width;
    const int rh = region.is_empty() ? static_cast<int>(reverse_vp.height) : region.height;
    pixels["width"] = static_cast<std::int64_t>(std::lround(static_cast<double>(rw) * reverse_scale));
    pixels["height"] = static_cast<std::int64_t>(std::lround(static_cast<double>(rh) * reverse_scale));
  }
  (void)actual_width;
  (void)actual_height;""",
        "capture_pixel_size_comes_from_the_export_not_the_request",
    ),
    (
        "圆角边框环：把内圈重建改回“丢起点 + 多发一个 Close”",
        PATH_CPP,
        """  Path inner_forward;\n  inner_forward.add_rounded_rect(inner_box, inner_radius);\n  path.add_path(reversed_closed_subpath(inner_forward));\n  return path;""",
        """  Path forward;\n  forward.add_rounded_rect(inner_box, inner_radius);\n  const auto commands = forward.commands();\n  for (std::size_t index = commands.size(); index-- > 0;) {\n    const PathCommand& command = commands[index];\n    switch (command.kind) {\n      case PathCommand::Kind::MoveTo: path.line_to(command.p1); break;\n      case PathCommand::Kind::LineTo: path.line_to(command.p1); break;\n      case PathCommand::Kind::QuadTo: path.quad_to(command.p2, command.p1); break;\n      case PathCommand::Kind::CubicTo: path.cubic_to(command.p3, command.p2, command.p1); break;\n      case PathCommand::Kind::Close: path.close(); break;\n    }\n  }\n  (void)&reversed_closed_subpath;\n  return path;""",
        "border_ring_arcs_carry_same_ink_as_stroke",
    ),
    (
        "斜向带：把逐像素累加覆盖率改回“逐 run 直接混合”（斜边系统性欠墨）",
        CANVAS_CPP,
        "  if (solid && !masked && blend == BlendMode::SrcOver) {\n    int lo = physical_width_;",
        "  if (false) {\n    int lo = physical_width_;",
        "diagonal_band_ink_is_angle_independent",
    ),
    (
        "遮罩光栅化：把覆盖率累加改回“逐段取最大值”（弧上偏淡）",
        RASTERIZER_CPP,
        "                              accumulated[static_cast<std::size_t>(x - lo)] += (right - left) * weight;",
        "                              accumulated[static_cast<std::size_t>(x - lo)] = std::max(\n                                  accumulated[static_cast<std::size_t>(x - lo)], (right - left) * weight);",
        "border_ring_total_ink_matches_perimeter_times_width",
    ),
]


def run_test(name: str) -> tuple[bool, str]:
    # ⚠ `text=True` 会用**系统编码**解码：Windows 上是 GBK，而 `st` / 测试输出是 UTF-8
    # （带方框字符、✓ 等）——实测直接 `UnicodeDecodeError` 在读取线程里崩，
    # 而且因为那是后台线程，异常不会让 `subprocess.run` 失败，只会让 `stdout` 变 `None`。
    # 因此显式按 UTF-8 解码、`errors="replace"`。
    proc = subprocess.run(
        ["./build/bin/st", "test", "--profile", "debug", name],
        capture_output=True, timeout=900,
    )
    out = (proc.stdout or b"").decode("utf-8", "replace") + (proc.stderr or b"").decode(
        "utf-8", "replace")
    # ⚠ 编译失败时 `st test` 不一定给出非零退出码，而测试**没跑**——
    # 沿用上一份二进制的状态下，本应变红的用例会“意外”保持绿。
    # 本脚本自己踩过这个坑（回退文本引入 -Werror 错误），因此先查编译段落。
    if "编译失败" in out or "error:" in out:
        return True, out  # passed=True → 调用方报“仍然是绿的”并指出问题
    # ⚠ **进程撞断言崩溃也算“没抓住”**：crash 时既没有 PASS 也没有 FAIL，
    # 旧口径会把它当成“仍然绿”——实测就因此把一条**原版也崩**的用例
    # （`capture_pixel_size_comes_from_the_export_not_the_request`）报成“回退后仍绿”，
    # 白白指向错误的嫌疑人。
    #
    # ⚠ 判据只能用「输出里的崩溃字样」，**不能看退出码**：测试**正常变红**时
    # `st test` 也返回非零（本仓实测），拿退出码当依据会把每一条都误报成“没抓住”。
    if "Assertion failed" in out or "Aborted" in out or "段错误" in out:
        return True, out
    failed = bool(re.search(r"FAIL", out)) or " 1 failed" in out
    return (not failed), out


def main() -> int:
    ok = True
    # 自报条目数（文档不写死这个数字，否则改一点就漂）。
    print(f"逆向验证：{len(CASES)} 条关键契约\n")
    for label, path, original, reverted, test_name in CASES:
        text = open(path, encoding="utf-8").read()
        if original not in text:
            print(f"[跳过] {label}\n       原文未找到（可能已被改动）：{original[:60]!r}")
            ok = False
            continue
        backup = path + ".reverse-backup"
        shutil.copy(path, backup)
        try:
            open(path, "w", encoding="utf-8").write(text.replace(original, reverted, 1))
            passed, out = run_test(test_name)
            if passed:
                print(f"[红][失败] {label}\n       回退后「{test_name}」**仍然是绿的**——这条测试抓不住这个缺陷")
                ok = False
            else:
                print(f"[绿][通过] {label}\n       回退后「{test_name}」按预期变红")
        finally:
            # ⚠ **必须先删旧文件再写新内容，不能只用 move/copy 覆盖**。
            #
            # 本仓库的增量构建按 **mtime** 判断是否重编（见 `docs/BACKLOG.md` 里
            # "增量构建会漏掉改动"那条），而 `shutil.move`/`copy2` 会把备份时
            # **原有的旧 mtime 一起搬回来**——比刚才编译回退版生成的 `.o` 还旧。
            # 于是下一轮构建认为源码未变，**继续用回退版的二进制**，表现为
            # "单跑绿、全量跑红"：全量用例编到了回退码，而单跑因缓存命中而骗过。
            # 删后重建会拿到当前时间，构建才会真的重编。
            content = open(backup, encoding="utf-8").read()
            # ⚠ Windows 上刚写完的文件可能还被句柄占着（`os.remove` 报 WinError 32），
            # 而这一步在 `finally` 里——一旦抩出去，**源码会永久停在回退状态**。
            # 实测踩到：脚本崩在第 11 条，`src/control/server.cpp` 留在回退版，
            # 后续所有构建都在用错误实现（而当时没有任何红灯）。
            try:
                os.remove(path)
            except OSError:
                pass
            open(path, "w", encoding="utf-8").write(content)
            try:
                os.remove(backup)
            except OSError:
                pass
            # 无论恢复是否顺利，都校验文件真的回到了原文——这是最后一道保险。
            if original not in open(path, encoding="utf-8").read():
                print(f"[致命] {path} 未能恢复原状！请手工从 git 恢复后再跑。")
                return 1
    if not ok:
        print("\n存在未能抓住缺陷的用例，需要修测试或修实现。")
        return 1
    print(f"\n全部 {len(CASES)} 条都通过了逆向验证：回退即变红。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
