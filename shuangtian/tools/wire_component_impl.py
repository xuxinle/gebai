#!/usr/bin/env python3
#
# ⚠ **一次性迁移脚本（2026-11-08 已执行完毕）**
#
# 它给拆出来的基础组件在各自实现文件里追加登记定义。留在仓库里的理由只有一个：
# 它记录了"哪个组件在哪个实现文件里"这份对应关系——将来若要再拆/合并组件，
# 这份映射是最快的起点，比重新 grep 一遍可靠。
#
# 重新执行前请先读 `include/st/ui/components/README.md`：现在的机制已经定型，
# 脚本里的做法（追加 `ST_UI_COMPONENT` / `ST_UI_COMPONENT_IMPL`）仍然正确，
# 但它只覆盖当年那一批组件，**不会**自动发现新组件。

#!/usr/bin/env python3
"""给拆出的组件头补上 `.cpp` 侧的登记定义（`ST_UI_COMPONENT_IMPL`）。

## 为什么需要这一步

`ST_UI_COMPONENT(TYPE, NAME)` 只在头里**声明**一个导入函数（见
`st/ui/component_registry.hpp` 文件头：静态初始化在静态库里不可靠，实测过三条死路）。
真正的登记写在组件实现的 `.cpp` 里，由 `ST_UI_COMPONENT_IMPL_WITH` 生成。

本脚本把那一行追加到每个已拆组件的 `.cpp` 末尾（幂等：已经有就跳过）。

## 用法

    py -3 tools/wire_component_impl.py --dry
    py -3 tools/wire_component_impl.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMP_INC = ROOT / "include" / "st" / "ui" / "components"
COMP_SRC = ROOT / "src" / "ui" / "components"

# 组件头 → (类名或表达式, 注册名, 实现文件)
# 实现文件与头不同名的情况要显式写（例如 select_box.hpp → select.cpp）。
WIRING: list[tuple[str, str, str, str]] = [
    ("text.hpp", "::st::ui::Text", "Text", "basic.cpp"),
    ("heading.hpp", "::st::ui::Heading", "Heading", "basic.cpp"),
    ("icon_view.hpp", "::st::ui::IconView", "Icon", "basic.cpp"),
    ("button.hpp", "::st::ui::Button", "Button", "basic.cpp"),
    ("card.hpp", "::st::ui::Card", "Card", "basic.cpp"),
    ("divider.hpp", "::st::ui::Divider", "Divider", "basic.cpp"),
    ("key_value_row.hpp", "::st::ui::KeyValueRow", "KeyValueRow", "basic.cpp"),
    ("progress_bar.hpp", "::st::ui::ProgressBar", "ProgressBar", "feedback.cpp"),
    ("spinner.hpp", "::st::ui::Spinner", "Spinner", "feedback.cpp"),
    ("badge.hpp", "::st::ui::Badge", "Badge", "feedback.cpp"),
    ("chip.hpp", "::st::ui::Chip", "Chip", "feedback.cpp"),
    ("avatar.hpp", "::st::ui::Avatar", "Avatar", "feedback.cpp"),
    ("tooltip.hpp", "::st::ui::Tooltip", "Tooltip", "feedback.cpp"),
    ("checkbox.hpp", "::st::ui::Checkbox", "Checkbox", "toggle.cpp"),
    ("radio.hpp", "::st::ui::Radio", "Radio", "toggle.cpp"),
    ("switch.hpp", "::st::ui::Switch", "Switch", "toggle.cpp"),
    ("input_box.hpp", "::st::ui::Input", "Input", "input.cpp"),
    ("text_area.hpp", "::st::ui::TextArea", "TextArea", "input.cpp"),
    ("list_item.hpp", "::st::ui::ListItem", "ListItem", "list.cpp"),
    ("list_view.hpp", "::st::ui::List", "List", "list.cpp"),
    ("dialog.hpp", "::st::ui::Dialog", "Dialog", "overlay.cpp"),
    ("toast.hpp", "::st::ui::Toast", "Toast", "overlay.cpp"),
    ("select_panel.hpp", "::st::ui::SelectPanel", "SelectPanel", "select.cpp"),
    ("select_box.hpp", "::st::ui::Select", "Select", "select.cpp"),
    ("scroll_bar.hpp", "::st::ui::ScrollBar", "ScrollBar", "scroll.cpp"),
    ("scroll_view.hpp", "::st::ui::ScrollView", "ScrollView", "scroll.cpp"),
]


def macro_for(cls: str, registered: str) -> str:
    """返回该组件在 .cpp 里要追加的登记行。"""
    # 需要构造参数的组件（与头里的 ST_UI_COMPONENT_WITH 保持一致）
    defaults = {
        "::st::ui::Button": 'std::make_unique<::st::ui::Button>("")',
        "::st::ui::KeyValueRow": 'std::make_unique<::st::ui::KeyValueRow>("", "")',
        "::st::ui::Checkbox": 'std::make_unique<::st::ui::Checkbox>("")',
        "::st::ui::Radio": 'std::make_unique<::st::ui::Radio>("")',
        "::st::ui::Switch": 'std::make_unique<::st::ui::Switch>("")',
        "::st::ui::ListItem": 'std::make_unique<::st::ui::ListItem>("", "")',
        "::st::ui::SelectPanel":
            'std::make_unique<::st::ui::SelectPanel>(std::vector<::st::ui::SelectOption>{}, '
            'std::nullopt)',
    }
    if cls in defaults:
        return (f"ST_UI_COMPONENT_IMPL_WITH({cls}, {registered!r}, "
                f"{defaults[cls]})")
    return f"ST_UI_COMPONENT_IMPL({cls})"


def main() -> int:
    dry = "--dry" in sys.argv
    by_file: dict[str, list[str]] = {}
    for _header, cls, registered, impl in WIRING:
        by_file.setdefault(impl, []).append(macro_for(cls, registered))

    for impl, lines in by_file.items():
        target = COMP_SRC / impl
        if not target.is_file():
            print(f"[skip] 缺少实现文件 {impl}", file=sys.stderr)
            continue
        text = target.read_text(encoding="utf-8")
        if "ST_UI_COMPONENT_IMPL" in text:
            print(f"[skip] {impl} 已有登记")
            continue
        # 只保留真正需要改名的（其余用 ST_UI_COMPONENT_IMPL）——这里统一展开，
        # 便于阅读与 grep。
        block = (
            "\n// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────\n"
            "//\n"
            "// 为什么登记写在实现文件里：`ST_UI_COMPONENT` 只在头里**声明**一个导入函数，\n"
            "// 真正的登记在这行宏展开出的函数体里。被调用才登记 —— 于是「导入」是一次\n"
            "// 真实调用，对链接器是「这个 .o 被需要」的铁证（静态初始化做不到这一点，\n"
            "// 见 component_registry.hpp 文件头的实测记录）。\n"
            + "\n".join(lines)
            + "\n"
        )
        if dry:
            print(f"[dry] {impl}: +{len(lines)} 行登记")
            continue
        target.write_text(text.rstrip("\n") + "\n" + block, encoding="utf-8")
        print(f"{impl}: +{len(lines)} 行登记")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
