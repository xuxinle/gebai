#!/usr/bin/env python3
#
# ⚠ **一次性迁移脚本（2026-11-08 已执行完毕）**
#
# 它给未拆分的组件（terminal / table / three_view 等）补上声明与登记。留在仓库里的理由只有一个：
# 它记录了"哪个组件在哪个实现文件里"这份对应关系——将来若要再拆/合并组件，
# 这份映射是最快的起点，比重新 grep 一遍可靠。
#
# 重新执行前请先读 `include/st/ui/components/README.md`：现在的机制已经定型，
# 脚本里的做法（追加 `ST_UI_COMPONENT` / `ST_UI_COMPONENT_IMPL`）仍然正确，
# 但它只覆盖当年那一批组件，**不会**自动发现新组件。

#!/usr/bin/env python3
"""给**尚未拆分**的组件头补上自注册声明，并给其实现文件补上登记定义。

## 背景

`st/ui/components/` 在 2026-11-08 拆成"一组件一头"之后，只有基础组件族
（`basic` / `feedback` / `toggle` / `input` / `list` / `overlay` / `select` / `scroll`）
完成了拆分。其余组件（`terminal` / `table` / `three_view` …）本来就"一个文件一个
主组件"，不需要拆——但它们同样需要**登记**，否则 `dsl::make_element("Terminal")`
查不到（而册府的桥函数会如实报"未注册"）。

本脚本对每个这样的头：
1. 在文件末尾追加 `ST_UI_COMPONENT(_WITH)(...)`（若还没有）；
2. 在其实现 `.cpp` 末尾追加 `ST_UI_COMPONENT_IMPL(_WITH)(...)`（若还没有）。

**幂等**：已有登记则跳过。

## 用法

    py -3 tools/wire_remaining_components.py --dry
    py -3 tools/wire_remaining_components.py
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INC = ROOT / "include" / "st" / "ui" / "components"
SRC = ROOT / "src" / "ui" / "components"

# (头文件, 类名, 注册名, 构造表达式或 None, 实现文件)
# 构造表达式为 None 时用 `std::make_unique<TYPE>()`。
COMPONENTS: list[tuple[str, str, str, str | None, str]] = [
    ("code_editor.hpp", "CodeEditor", "CodeEditor", None, "code_editor.cpp"),
    ("color_picker.hpp", "ColorPicker", "ColorPicker", None, "color_picker.cpp"),
    ("command_palette.hpp", "CommandPalette", "CommandPalette", None, "command_palette.cpp"),
    ("completion_popup.hpp", "CompletionPopup", "CompletionPopup", None, "completion_popup.cpp"),
    ("diff_view.hpp", "DiffView", "DiffView", None, "diff_view.cpp"),
    ("file_dialog.hpp", "FileDialog", "FileDialog", None, "file_dialog.cpp"),
    ("markdown_view.hpp", "MarkdownView", "MarkdownView", None, "markdown_view.cpp"),
    ("menu.hpp", "MenuPanel", "MenuPanel",
     "std::make_unique<::st::ui::MenuPanel>(std::vector<::st::ui::MenuItem>{})", "menu.cpp"),
    ("menu.hpp", "MenuBar", "MenuBar", None, "menu.cpp"),
    ("menu.hpp", "ContextMenu", "ContextMenu",
     "std::make_unique<::st::ui::ContextMenu>(::st::ui::math::Point{},\n"
     "        std::vector<::st::ui::MenuItem>{})", "menu.cpp"),
    ("merge_view.hpp", "MergeView", "MergeView", None, "merge_view.cpp"),
    ("slider.hpp", "Slider", "Slider", None, "slider.cpp"),
    ("split_view.hpp", "SplitView", "SplitView", None, "split_view.cpp"),
    ("table.hpp", "Table", "Table", None, "table.cpp"),
    ("tabs.hpp", "Tabs", "Tabs", None, "tabs.cpp"),
    ("terminal.hpp", "Terminal", "Terminal", None, "terminal.cpp"),
    ("theme_picker.hpp", "ThemePicker", "ThemePicker", None, "theme_picker.cpp"),
    ("timeline_view.hpp", "TimelineView", "TimelineView", None, "timeline_view.cpp"),
    ("title_bar.hpp", "TitleBar", "TitleBar", None, "title_bar.cpp"),
    ("tree.hpp", "Tree", "Tree", None, "tree.cpp"),
    ("viewport.hpp", "Viewport", "Viewport", None, "viewport.cpp"),
    ("window_frame.hpp", "WindowFrame", "WindowFrame", None, "window_frame.cpp"),
]

# 这些**不登记**（并在头里写清理由）：
# - SceneView / ThreeView / LabelOverlay / GraphView：需要真实三维后端，
#   历史上无头环境会崩；`ThreeView` 头注已写明"它不进可声明组件清单"。
# - SlowContent：它不是独立可视组件（是异步排版的宿主），由 MarkdownView 内用。
SKIPPED = {
    "scene_view.hpp": "需要真实三维后端（无头环境曾段错误），不进声明式注册表",
    "three_view.hpp": "同上；册府/建木由 C++ 直接 new 并挂树",
    "label_overlay.hpp": "三维叠层（依赖视口投影器），由 GraphView 内用",
    "graph_view.hpp": "同上（组合 ThreeView + LabelOverlay）",
    "slow_content.hpp": "异步排版宿主，由 MarkdownView 内用，不单独声明",
}


def decl(cls: str, registered: str, make: str | None) -> str:
    if make is None:
        return f"ST_UI_COMPONENT(::st::ui::{cls}, \"{registered}\")"
    return f"ST_UI_COMPONENT_WITH(::st::ui::{cls}, \"{registered}\", {make})"


def impl(cls: str, registered: str, make: str | None) -> str:
    if make is None:
        return f"ST_UI_COMPONENT_IMPL(::st::ui::{cls})"
    return f"ST_UI_COMPONENT_IMPL_WITH(::st::ui::{cls}, \"{registered}\", {make})"


def main() -> int:
    dry = "--dry" in sys.argv
    # 头：按文件聚合（一个头可能有多行登记）
    headers: dict[str, list[str]] = {}
    impls: dict[str, list[str]] = {}
    for header, cls, registered, make, impl_file in COMPONENTS:
        headers.setdefault(header, []).append(decl(cls, registered, make))
        impls.setdefault(impl_file, []).append(impl(cls, registered, make))

    for header, lines in headers.items():
        target = INC / header
        if not target.is_file():
            print(f"[skip] 缺少头 {header}", file=sys.stderr)
            continue
        text = target.read_text(encoding="utf-8")
        if "ST_UI_COMPONENT" in text:
            print(f"[skip] {header} 已有登记")
            continue
        block = (
            "\n// ── 自注册声明（`st/ui/component_registry.hpp`）───────────────────────\n"
            "// 只声明导入函数；真正的登记在实现文件里（静态初始化在静态库中不可靠，\n"
            "// 见 component_registry.hpp 文件头的实测记录）。\n"
            "//\n"
            "// ⚠ include 本头 + 调用 `st::ui::import_components<类名>()` 才真正可用；\n"
            "//   `st/ui/components/all.hpp` 一次做完全部导入。\n"
            "#include \"st/ui/component_registry.hpp\"\n\n"
            + "\n".join(lines)
            + "\n"
        )
        if dry:
            print(f"[dry] {header}: +{len(lines)} 行声明")
            continue
        target.write_text(text.rstrip("\n") + "\n" + block, encoding="utf-8")
        print(f"{header}: +{len(lines)} 行声明")

    for impl_file, lines in impls.items():
        target = SRC / impl_file
        if not target.is_file():
            print(f"[skip] 缺少实现 {impl_file}", file=sys.stderr)
            continue
        text = target.read_text(encoding="utf-8")
        if "ST_UI_COMPONENT_IMPL" in text:
            print(f"[skip] {impl_file} 已有登记")
            continue
        block = (
            "\n// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────\n"
            + "\n".join(lines)
            + "\n"
        )
        if dry:
            print(f"[dry] {impl_file}: +{len(lines)} 行登记")
            continue
        target.write_text(text.rstrip("\n") + "\n" + block, encoding="utf-8")
        print(f"{impl_file}: +{len(lines)} 行登记")

    for header, reason in SKIPPED.items():
        print(f"[不登记] {header}：{reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
