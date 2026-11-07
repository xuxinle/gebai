#!/usr/bin/env python3
"""逆向验证：把「窗口居中」与「主题常用性排序」的修复逐条回退，确认回归用例真的变红。

## 为什么必须做这一步

`CONVENTIONS.md` §7.1 / 本子Agent 提示词第 11 条：**测试通过不等于测试有效**。
一条恒绿的护栏看起来像护栏，实际什么都拦不住。实测踩过多次：
- 差异导航按钮的 `disabled` 判据回退 → 立刻变红（真的钉住了）；
- 光标跳行的 E2E 回退 → **没变红**（测试量不到那个入口）。

本脚本按「改坏一处 → 跑对应用例 → 断言变红 → 恢复」逐条验证，任一条不变红即视为
**测试无效**并如实报告。

## 用法

    python tools/center_theme_order_reverse_verify.py [--profile dev]

退出码 0 = 全部条目都按预期变红（护栏有效）；非 0 = 有条目没变红或恢复失败。
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# 每条：名称 / 相对路径 / 原文 / 改坏后的文本 / 期望变红的用例（`st test <filter>`）
CASES = [
    {
        "name": "居中：剩余空间不再折半（窗口会贴左上角）",
        "file": "src/shell/backend.cpp",
        "old": "  const int slack_x = (work_area.width - window.width) / 2;\n"
               "  const int slack_y = (work_area.height - window.height) / 2;",
        "new": "  const int slack_x = (work_area.width - window.width) / 3;\n"
               "  const int slack_y = (work_area.height - window.height) / 3;",
        "filter": "centered_origin",
    },
    {
        "name": "居中：丢掉副屏工作区偏移（副屏上的窗口会被搬到主屏）",
        "file": "src/shell/backend.cpp",
        "old": "  return math::IntPoint{work_area.x + offset_x, work_area.y + offset_y};",
        "new": "  return math::IntPoint{offset_x, offset_y};",
        "filter": "centered_origin_honors_secondary_monitor_offset",
    },
    {
        "name": "居中：摆不下时不夹取（窗口被推到屏外，得到负坐标）",
        "file": "src/shell/backend.cpp",
        "old": "  const int offset_x = slack_x > 0 ? slack_x : 0;\n"
               "  const int offset_y = slack_y > 0 ? slack_y : 0;",
        "new": "  const int offset_x = slack_x;\n"
               "  const int offset_y = slack_y;",
        "filter": "centered_origin_clamps_oversized_window_into_work_area",
    },
    {
        "name": "无头后端假装能居中（返回 ok 而不是 Unsupported）",
        "file": "src/shell/backend.cpp",
        "old": "  /// 无头没有“屏幕位置”这个概念：如实报 `Unsupported`（不假装做了）。\n"
               "  auto center_window() -> Status override {\n"
               "    return unexpected(ErrorCode::Unsupported, \"无头后端没有屏幕位置\");\n"
               "  }",
        "new": "  /// 无头没有“屏幕位置”这个概念：如实报 `Unsupported`（不假装做了）。\n"
               "  auto center_window() -> Status override {\n"
               "    return ok();\n"
               "  }",
        "filter": "headless_backend_reports_no_window_position",
    },
    {
        "name": "主题顺序：选择器改回字母序（默认两档被挤到右半边）",
        "file": "src/ui/components/theme_picker.cpp",
        "old": "  for (const auto name : builtin_theme_names_by_popularity()) names_.emplace_back(name);",
        "new": "  for (const auto name : builtin_theme_names()) names_.emplace_back(name);",
        "filter": "theme_picker_",
    },
    {
        # 回退必须**能编译过**（CONVENTIONS §7.3：`-Werror` 把编译打断 ⇒ 测试跑的是
        # 上一份二进制 ⇒ 假绿）。第一版写成 `const std::vector<std::string_view> order{};`
        # 后就因 `popularity_order` 变成未使用的静态函数而编译失败，实测踩到。
        "name": "主题顺序：排序表恒等于字母序（表本身没生效）",
        "file": "src/ui/builtin_themes.cpp",
        "old": "  for (const std::string_view name : order) {\n"
               "    if (std::find(names.begin(), names.end(), name) == names.end()) continue;\n"
               "    sorted.push_back(name);\n"
               "  }",
        "new": "  for (const std::string_view name : order) {\n"
               "    (void)name;\n"
               "    break;\n"
               "  }",
        "filter": "builtin_theme_names_by_popularity",
    },
    {
        "name": "主题顺序：未登记的名字被丢掉（新增主题从选择器里消失）",
        "file": "src/ui/builtin_themes.cpp",
        "old": "  for (const std::string_view name : names) {\n"
               "    if (std::find(sorted.begin(), sorted.end(), name) != sorted.end()) continue;\n"
               "    sorted.push_back(name);\n"
               "  }",
        "new": "  for (const std::string_view name : names) {\n"
               "    (void)name;\n"
               "    break;\n"
               "  }",
        "filter": "sort_theme_names_keeps_unlisted_entries_at_tail",
    },
]


def run_st(args: list[str]) -> subprocess.CompletedProcess[str]:
    exe = ROOT / "build" / "bin" / ("st.exe" if sys.platform == "win32" else "st")
    return subprocess.run([str(exe), *args], cwd=ROOT, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def main() -> int:
    # Windows 控制台默认 GBK：中文与 ✓/✗ 会抛 UnicodeEncodeError（实测踩到，
    # 它会把“验证结论”变成一次 traceback，比不打印更糟）。
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", default="dev")
    args = parser.parse_args()

    failures: list[str] = []
    for index, case in enumerate(CASES, start=1):
        path = ROOT / case["file"]
        original = path.read_text(encoding="utf-8")
        if case["old"] not in original:
            failures.append(f"[{index}] {case['name']}：原文没找到（实现已变，脚本需更新）")
            print(f"[{index}] {case['name']}\n    ✗ 原文没找到，跳过（先核对 {case['file']}）")
            continue
        try:
            path.write_text(original.replace(case["old"], case["new"], 1), encoding="utf-8")
            built = run_st(["build", "--profile", args.profile])
            if built.returncode != 0:
                # 编译不过也算"没变红"——那是假绿的一种（测试跑的是旧二进制）
                failures.append(f"[{index}] {case['name']}：回退后编译失败（判据无效）")
                print(f"[{index}] {case['name']}\n    ✗ 回退后编译失败：\n{built.stdout[-1500:]}")
                continue
            result = run_st(["test", case["filter"], "--profile", args.profile])
            output = result.stdout + result.stderr
            failed_count = " 0 failed" not in output
            if failed_count:
                print(f"[{index}] {case['name']}\n    ✓ 变红（护栏有效）")
            else:
                failures.append(f"[{index}] {case['name']}：回退后仍然全绿（测试无效）")
                print(f"[{index}] {case['name']}\n    ✗ 仍然全绿——这条测试量不到该缺陷")
        finally:
            path.write_text(original, encoding="utf-8")

    # 恢复后跑一遍，确认回到全绿（否则"变红"可能只是环境坏了）
    restored = run_st(["test", "centered_origin", "--profile", args.profile])
    restored_theme = run_st(["test", "theme_picker_", "--profile", args.profile])
    for label, result in (("centered_origin", restored), ("theme_picker_", restored_theme)):
        output = result.stdout + result.stderr
        if " 0 failed" not in output:
            failures.append(f"恢复后 {label} 仍红：恢复失败或环境已坏")

    if failures:
        print("\n===== 逆向验证未通过 =====")
        for line in failures:
            print(" ·", line)
        return 1
    print("\n===== 逆向验证通过：全部条目都如实变红 =====")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
