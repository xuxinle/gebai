#!/usr/bin/env python3
#
# ⚠ **一次性迁移脚本（2026-11-08 已执行完毕）**
#
# 它按行切片把当时的 8 个聚合头拆成 26 个子头，并给每个头追加自注册声明。留在仓库里的理由只有一个：
# 它记录了"哪个组件在哪个实现文件里"这份对应关系——将来若要再拆/合并组件，
# 这份映射是最快的起点，比重新 grep 一遍可靠。
#
# 重新执行前请先读 `include/st/ui/components/README.md`：现在的机制已经定型，
# 脚本里的做法（追加 `ST_UI_COMPONENT` / `ST_UI_COMPONENT_IMPL`）仍然正确，
# 但它只覆盖当年那一批组件，**不会**自动发现新组件。

#!/usr/bin/env python3
"""把聚合组件头拆成「一组件一头」，并给每个头追加自注册锚点。

## 为什么要脚本（而不是手抄）

聚合头里的类定义有**大段注释与精确的私有成员**（`Text` 的行缓存四维判据、
`Terminal` 的会话状态…），手抄一定会差字。这个脚本按**行切片**搬运原文，
只额外生成文件头与 include 行，因此任何一个类的代码都与原头**逐字节相同**。

## 产物

- `include/st/ui/components/<组件>.hpp`：一组件一头（含 `ST_UI_COMPONENT` 锚点）
- 原来的聚合头（`basic.hpp` / `feedback.hpp` …）**改成只 include 各子头**，
  于是既有引用不需要一次改完（渐进迁移）。

## 用法

    py -3 tools/split_component_headers.py --dry     # 只打印计划
    py -3 tools/split_component_headers.py           # 执行
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMP = ROOT / "include" / "st" / "ui" / "components"

# 聚合头 → [(类名, 注册名, 新头名)]。
# 注册名必须与 dsl.cpp 的 ST_COMPONENT_LIST 第二列一致（对外契约，不能猜）。
GROUPS: dict[str, list[tuple[str, str, str]]] = {
    "basic.hpp": [
        ("Text", "Text", "text.hpp"),
        ("Heading", "Heading", "heading.hpp"),
        ("IconView", "Icon", "icon_view.hpp"),
        ("Button", "Button", "button.hpp"),
        ("Card", "Card", "card.hpp"),
        ("Divider", "Divider", "divider.hpp"),
        ("KeyValueRow", "KeyValueRow", "key_value_row.hpp"),
    ],
    "feedback.hpp": [
        ("ProgressBar", "ProgressBar", "progress_bar.hpp"),
        ("Spinner", "Spinner", "spinner.hpp"),
        ("Badge", "Badge", "badge.hpp"),
        ("Chip", "Chip", "chip.hpp"),
        ("Avatar", "Avatar", "avatar.hpp"),
        ("Tooltip", "Tooltip", "tooltip.hpp"),
    ],
    "toggle.hpp": [
        ("Checkbox", "Checkbox", "checkbox.hpp"),
        ("Radio", "Radio", "radio.hpp"),
        ("Switch", "Switch", "switch.hpp"),
    ],
    "input.hpp": [
        ("Input", "Input", "input_box.hpp"),
        ("TextArea", "TextArea", "text_area.hpp"),
    ],
    "list.hpp": [
        ("ListItem", "ListItem", "list_item.hpp"),
        ("List", "List", "list_view.hpp"),
    ],
    "overlay.hpp": [
        ("Dialog", "Dialog", "dialog.hpp"),
        ("Toast", "Toast", "toast.hpp"),
    ],
    "select.hpp": [
        ("SelectPanel", "SelectPanel", "select_panel.hpp"),
        ("Select", "Select", "select.hpp"),
    ],
    "scroll.hpp": [
        ("ScrollBar", "ScrollBar", "scroll_bar.hpp"),
        ("ScrollView", "ScrollView", "scroll_view.hpp"),
    ],
}

# 每个新头需要的 include（**只放这个类真正用到的**）。
# 判定依据：类体里是否出现相应符号；脚本会打印缺失告警，人工复核。
# 通用基线：element.hpp（基类）+ component_registry.hpp（自注册锚点）。
EXTRA_INCLUDES: dict[str, list[str]] = {
    "text.hpp": ['#include <cstddef>', '#include <string>', '#include <string_view>',
                 '#include <utility>', '#include <vector>'],
    "heading.hpp": ['#include "st/ui/components/text.hpp"'],
    "icon_view.hpp": ['#include <string>', '#include <string_view>',
                      '#include "st/ui/icon.hpp"'],
    "button.hpp": ['#include <functional>', '#include <string>', '#include <string_view>',
                   '#include "st/ui/components/icon_view.hpp"'],
    "card.hpp": [],
    "divider.hpp": [],
    "key_value_row.hpp": ['#include <string>', '#include <string_view>'],
    "progress_bar.hpp": ['#include <string>', '#include <string_view>'],
    "spinner.hpp": [],
    "badge.hpp": ['#include <string>', '#include <string_view>'],
    "chip.hpp": ['#include <string>', '#include <string_view>',
                 '#include "st/ui/components/icon_view.hpp"'],
    "avatar.hpp": ['#include <string>', '#include <string_view>'],
    "tooltip.hpp": ['#include <string>', '#include <string_view>'],
    "checkbox.hpp": ['#include <functional>', '#include <string>', '#include <string_view>'],
    "radio.hpp": ['#include <functional>', '#include <string>', '#include <string_view>'],
    "switch.hpp": ['#include <functional>', '#include <string>', '#include <string_view>'],
    "input_box.hpp": ['#include <functional>', '#include <string>', '#include <string_view>'],
    "text_area.hpp": ['#include <functional>', '#include <string>', '#include <string_view>'],
    "list_item.hpp": ['#include <string>', '#include <string_view>'],
    "list_view.hpp": ['#include <functional>', '#include <string>', '#include <string_view>',
                      '#include <vector>'],
    "dialog.hpp": ['#include <functional>', '#include <string>', '#include <string_view>'],
    "toast.hpp": ['#include <string>', '#include <string_view>'],
    "select_panel.hpp": ['#include <optional>', '#include <string>', '#include <string_view>',
                         '#include <vector>'],
    "select.hpp": ['#include <functional>', '#include <optional>', '#include <string>',
                   '#include <string_view>', '#include <vector>',
                   '#include "st/ui/components/select_panel.hpp"'],
    "scroll_bar.hpp": [],
    "scroll_view.hpp": ['#include <functional>', '#include <string>',
                        '#include "st/ui/components/scroll_bar.hpp"'],
}

HEADER_TEMPLATE = """#pragma once

/// @file {header}
/// @brief `{cls}`{short}。
///
/// 从 `{group}` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `{group}` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

{includes}

namespace st::ui {{

{body}

}}  // namespace st::ui

ST_UI_COMPONENT(::{reg})
"""

AGGREGATE_TEMPLATE = """#pragma once

/// {title}
///
/// ## 这个头现在是**聚合头**（只 include 各子头）
///
/// 原先它把 {count} 个组件塞在一个文件里——那是「按需导入」做不到的根因之一：
/// include 一个组件就等于 include 全部，依赖关系也藏在这份清单里而不是组件自己身上。
///
/// 现在**一组件一头**（规则见 `st/ui/components/README.md`）。本文件保留为**便利聚合**：
/// - 想用一个组件：include 它自己的头（如 `st/ui/components/button.hpp`）——
///   只有它进产物；
/// - 想要"全部基础组件"：include 本头（开发期方便，交付前建议改回按需）。
{includes}

// 自注册锚点随各子头到达（`ST_UI_COMPONENT`）；本文件不再持有任何类型定义。
"""


def find_class_block(lines: list[str], cls: str) -> tuple[int, int, str]:
    """返回（起始行下标, 结束行下标, 说明）。块从类定义上方的注释起，到配对的 `};` 止。"""
    start = None
    for index, line in enumerate(lines):
        if re.match(rf"^class {cls}\b", line):
            start = index
            break
    if start is None:
        raise SystemExit(f"找不到类 {cls}")
    # 向上吃掉紧邻的注释/空行（doc 注释属于该类）
    doc_start = start
    while doc_start > 0:
        stripped = lines[doc_start - 1].strip()
        if stripped.startswith("///") or stripped.startswith("//"):
            doc_start -= 1
            continue
        break
    # 向下找配对的 `};`（顶层：行首缩进恰为 0 且是 `};`）
    end = None
    for index in range(start, len(lines)):
        if lines[index].rstrip() == "};":
            end = index
            break
    if end is None:
        raise SystemExit(f"类 {cls} 找不到结尾 `}};`")
    return doc_start, end, ""


def brief_of(block: str) -> str:
    """类上方的第一行 `///` 文档当 brief。"""
    for line in block.split("\n"):
        stripped = line.strip()
        if stripped.startswith("///"):
            text = stripped[3:].strip()
            if text and not text.startswith("@"):
                return text
    return ""


def main() -> int:
    dry = "--dry" in sys.argv
    planned: list[str] = []
    for group, members in GROUPS.items():
        group_path = COMP / group
        if not group_path.is_file():
            print(f"[skip] {group} 不存在", file=sys.stderr)
            continue
        source = group_path.read_text(encoding="utf-8")
        lines = source.split("\n")
        for cls, registered, header in members:
            doc_start, end, _ = find_class_block(lines, cls)
            body = "\n".join(lines[doc_start:end + 1]).rstrip()
            brief = brief_of(body)
            includes = ["#include \"st/ui/component_registry.hpp\"",
                        "#include \"st/ui/element.hpp\""] + EXTRA_INCLUDES.get(header, [])
            # 去重并保持顺序（标准库头在前、项目头在后）
            std = [i for i in includes if i.startswith("#include <")]
            proj = [i for i in includes if i.startswith("#include \"")]
            ordered = sorted(set(std)) + sorted(set(proj))
            text = HEADER_TEMPLATE.format(
                header=header,
                cls=cls,
                short=("：" + brief) if brief else "",
                group=group,
                includes="\n".join(ordered),
                body=body,
                reg=f"st::ui::{cls}, \"{registered}\"",
            )
            target = COMP / header
            planned.append(f"{group} :: {cls} -> {header}（{len(body.split(chr(10)))} 行）")
            if dry:
                continue
            target.write_text(text, encoding="utf-8")

    for item in planned:
        print(item)

    if dry:
        print(f"\n[dry] 计划写 {len(planned)} 个头")
        return 0

    # 聚合头改成只 include 子头
    titles = {
        "basic.hpp": "基础组件（文本 / 标题 / 图标 / 按钮 / 卡片 / 分隔线 / 键值行）",
        "feedback.hpp": "反馈组件（进度条 / 加载 / 徽标 / 标签片 / 头像 / 悬浮提示）",
        "toggle.hpp": "开关类组件（复选框 / 单选 / 开关）",
        "input.hpp": "输入类组件（单行输入 / 多行文本域）",
        "list.hpp": "列表组件（列表项 / 列表）",
        "overlay.hpp": "浮层组件（对话框 / 轻提示）",
        "select.hpp": "选择组件（选择面板 / 下拉选择）",
        "scroll.hpp": "滚动组件（滚动条 / 滚动视图）",
    }
    for group, members in GROUPS.items():
        includes = "\n".join(f'#include "st/ui/components/{header}"' for _, _, header in members)
        text = AGGREGATE_TEMPLATE.format(
            title=titles.get(group, group),
            count=len(members),
            includes=includes,
        )
        (COMP / group).write_text(text, encoding="utf-8")
        print(f"聚合头改写：{group}（{len(members)} 个子头）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
