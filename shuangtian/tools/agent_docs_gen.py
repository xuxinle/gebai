"""生成 `docs/agent-docs/components/<族>/<组件>.md` —— 组件详情页。

## 为什么要有这个脚本

组件详情的**事实来源是组件头/实现文件本身**（构造签名、属性面、动作面、头注释里的取舍说明）。
人手抄一遍必然漂移：新增属性忘了写、动作改名没跟着改。所以详情页由本脚本从源码抽取生成，
`tools/check_agent_docs.py` 与智能体侧测试**双向核对**（页数、组件覆盖、路径可达）。

## 分工（谁写什么）

| 内容 | 谁写 |
|---|---|
| 每组件一页的机械事实（构造/属性/动作/源码位置/头注小节目录） | **本脚本**（重跑即同步） |
| 族页（怎么选控件、组合套路、坑） | 手写（`components/<族>.md`，本脚本**不碰**） |
| 根入口与 `overview/` 各篇（架构/构建/协议/验收） | 手写（本脚本**不碰**） |

脚本只写 `components/<族>/` 下的文件，因此重跑不会覆盖任何手写内容。

用法（cwd = 仓库根 `shuangtian/`）：

```sh
python tools/agent_docs_gen.py            # 重新生成
python tools/agent_docs_gen.py --check    # 只核对（不写），有漂移则退出码 1
```
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

ROOT = Path(".")
HPP_DIR = ROOT / "include" / "st" / "ui" / "components"
CPP_DIR = ROOT / "src" / "ui" / "components"
OUT_DIR = ROOT / "docs" / "agent-docs" / "components"

# 组件 → 族。**必须覆盖注册表里的每一个组件**（漏了测试会红）。
# 前九个族与聚合头同名（`basic.hpp` / `feedback.hpp` / …），其余族是按用途分的，
# 它们的成员不在任何聚合头里——这是分组，不是产物的边界。
FAMILY: dict[str, str] = {
    # basic.hpp
    "Text": "basic", "Heading": "basic", "Icon": "basic", "Button": "basic",
    "Card": "basic", "Divider": "basic", "KeyValueRow": "basic",
    "DividerLabel": "basic", "Kbd": "basic", "Link": "basic",
    # feedback.hpp
    "ProgressBar": "feedback", "Spinner": "feedback", "Badge": "feedback",
    "Chip": "feedback", "Avatar": "feedback", "Tooltip": "feedback",
    "AvatarGroup": "feedback", "Banner": "feedback", "EmptyState": "feedback",
    "Skeleton": "feedback", "ProgressRing": "feedback", "Sparkline": "feedback",
    "StatusDot": "feedback", "Toast": "feedback",
    # toggle.hpp
    "Checkbox": "toggle", "Radio": "toggle", "Switch": "toggle", "RadioGroup": "toggle",
    # input.hpp
    "Input": "input", "TextArea": "input", "SearchBox": "input",
    "NumberInput": "input", "Slider": "input", "Rating": "input",
    "ColorPicker": "input", "TagInput": "input", "FormRow": "input",
    "SegmentedControl": "input",
    # select.hpp
    "Select": "select", "SelectPanel": "select",
    # list.hpp
    "List": "list", "ListItem": "list",
    # scroll.hpp
    "ScrollView": "scroll", "ScrollBar": "scroll",
    # overlay.hpp
    "Dialog": "overlay",
    # panel.hpp（定义在 element.hpp，本头只做登记）
    "Panel": "panel", "Spacer": "panel",
    # 导航与组织
    "Accordion": "navigation", "Breadcrumb": "navigation", "Pagination": "navigation",
    "Steps": "navigation", "Tabs": "navigation", "Toolbar": "navigation",
    "StatusBar": "navigation", "MenuPanel": "navigation", "MenuBar": "navigation",
    "ContextMenu": "navigation", "CommandPalette": "navigation", "Tree": "navigation",
    # 数据展示
    "Table": "data", "TimelineView": "data", "GraphView": "data",
    # 编辑器与文档视图
    "CodeEditor": "editor", "DiffView": "editor", "MergeView": "editor",
    "MarkdownView": "editor", "HtmlView": "editor", "Terminal": "editor",
    "FileDialog": "editor", "CompletionPopup": "editor",
    # 三维
    "ThreeView": "3d", "SceneView": "3d", "LabelOverlay": "3d", "Connector": "3d",
    # 布局与窗口
    "SplitView": "layout", "Viewport": "layout", "SlowContent": "layout",
    "ThemePicker": "layout", "WindowFrame": "layout", "TitleBar": "layout",
    # 原生子窗口宿主（可选能力，不可用时退化为占位块）
    "NativeHost": "layout",
    # 可编程嵌入浏览器（可选能力，不可用时退化为占位块）
    "WebView": "layout",
}

FAMILY_TITLE = {
    "basic": "基础展示", "feedback": "反馈与状态", "toggle": "开关与勾选",
    "input": "输入与取值", "select": "下拉选择", "list": "列表",
    "scroll": "滚动", "overlay": "浮层", "panel": "框架容器",
    "navigation": "导航与组织", "data": "数据展示", "editor": "编辑器与文档视图",
    "3d": "三维与连线", "layout": "布局与窗口",
}

# 属性面里"通用属性"（所有元素都有，由 `Element` 提供）——单独列出来，免得每页重复
UNIVERSAL = {
    "id", "visible", "enabled", "x", "y", "width", "height", "flex_grow", "flex_shrink",
    "padding", "padding_x", "padding_y", "margin", "gap", "align", "justify", "background",
    "border", "radius", "opacity", "style", "class", "key", "text", "tooltip",
}


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def class_doc_comment(text: str, cls: str) -> tuple[str, list[tuple[str, int]], int]:
    """取 `class <cls>` 之前的 `///` 注释块。

    返回 (首段摘要, [(小节标题, 行号)...], 起始行号)。
    **首段**= 注释块里第一个空注释行（`///`）之前的内容；小节 = 所有 `/// ## xxx`。
    """
    lines = text.splitlines()
    idx = None
    for i, line in enumerate(lines):
        if re.match(rf"^class\s+{re.escape(cls)}\b", line):
            idx = i
            break
    if idx is None:
        return "", [], 0
    # 向上收集连续的 /// 行（跳过 #include 等）
    block: list[tuple[int, str]] = []
    j = idx - 1
    while j >= 0:
        line = lines[j].strip()
        if line.startswith("///"):
            block.append((j + 1, line))
            j -= 1
            continue
        if line == "" or line.startswith("//") is False:
            break
    block.reverse()
    if not block:
        return "", [], idx + 1
    summary: list[str] = []
    sections: list[tuple[str, int]] = []
    empty_seen = 0
    for lineno, line in block:
        body = line[3:].strip()
        if body == "":
            empty_seen += 1
            continue
        m = re.match(r"^##\s+(.+)$", body)
        if m:
            sections.append((m.group(1).strip(), lineno))
            continue
        if empty_seen == 0:
            summary.append(body)
    # 首段可能只有一行标题式描述，也可能有多行；取前 6 行足够
    return " ".join(summary[:6]).strip(), sections, block[0][0]


def ctor_of(text: str, cls: str) -> str:
    """取第一个构造声明（跨行合并）。取不到返回空串。"""
    pattern = re.compile(rf"^\s*(?:explicit\s+)?{re.escape(cls)}\s*\(", re.M)
    m = pattern.search(text)
    if not m:
        return ""
    start = m.start()
    end = text.find(";", start)
    if end < 0:
        return ""
    decl = " ".join(text[start:end].split())
    # 只要形状 `Cls(args)`，去掉显式构造符与前导空白
    decl = decl.replace("explicit ", "").strip()
    return decl


def brace_block(text: str, needle: str, limit: int = 4000) -> str:
    """取 `needle` 之后第一个 `{...}`（含花括号）——用于读 `with_universal({...})` 之类。"""
    i = text.find(needle)
    if i < 0:
        return ""
    j = text.find("{", i)
    if j < 0 or j - i > limit:
        return ""
    depth = 0
    for k in range(j, min(len(text), j + limit)):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                return text[j + 1 : k]
    return ""


def props_of(cpp: str, cls: str) -> tuple[list[str], list[str]]:
    """从实现的 `Cls::property_names()` / `Cls::readonly_property_names()` 抽属性名。

    ⚠ **必须按类名定位**：一个 `.cpp` 里常放多个组件（`basic.cpp` 实现了十来个），
    不按类名找就会拿到“本文件第一个 property_names”——于是所有组件的属性面都变成同一个。
    """
    rw = re.findall(r'"([a-z][a-z0-9_.]*)"', brace_block(cpp, f"{cls}::property_names()"))
    ro = re.findall(r'"([a-z][a-z0-9_.]*)"', brace_block(cpp, f"{cls}::readonly_property_names()"))
    rw = [n for n in dict.fromkeys(rw)]
    ro = [n for n in dict.fromkeys(ro) if n not in rw]
    return rw, ro


def actions_of(cpp: str, cls: str) -> list[str]:
    """从 `Cls::invoke_action` 抽动作名（按类名定位，理由同上）。"""
    body = brace_block(cpp, f"{cls}::invoke_action", 20000)
    return list(dict.fromkeys(re.findall(r'action\s*==\s*"([a-z][a-z0-9_.]*)"', body)))


def impl_file(cls: str, hpp: Path) -> Path | None:
    """定位类的实现文件：先看同名 `.cpp`，再全目录搜 `Cls::` 定义。"""
    direct = CPP_DIR / f"{hpp.stem}.cpp"
    if direct.exists() and f"{cls}::" in read(direct):
        return direct
    for cpp in sorted(CPP_DIR.glob("*.cpp")):
        if f"{cls}::" in read(cpp):
            return cpp
    return direct if direct.exists() else None


def design_refs(text: str) -> list[str]:
    refs = re.findall(r"`?DESIGN\.md`?\s*§\s*(\d+(?:\.\d+)*)", text)
    return list(dict.fromkeys(refs))


def registry_items() -> list[dict]:
    """扫全部组件头，取 `ST_UI_COMPONENT*(类型, "注册名")` 与类名 → 文件。"""
    items: list[dict] = []
    for hpp in sorted(HPP_DIR.glob("*.hpp")):
        text = read(hpp)
        for m in re.finditer(r'ST_UI_COMPONENT(?:_WITH)?\(\s*::st::ui::(\w+)\s*,\s*"([^"]+)"', text):
            items.append({"cls": m.group(1), "name": m.group(2), "hpp": hpp, "text": text, "registered": True})
    return items


# 不进注册表、但确实存在且值得一页的组件（三维族等）。它们的头末尾写了理由。
UNREGISTERED = {
    "ThreeView": "three_view.hpp",
    "SceneView": "scene_view.hpp",
    "LabelOverlay": "label_overlay.hpp",
    "GraphView": "graph_view.hpp",
}


def unregistered_items() -> list[dict]:
    items: list[dict] = []
    for cls, filename in UNREGISTERED.items():
        hpp = HPP_DIR / filename
        if not hpp.exists():
            continue
        items.append({"cls": cls, "name": cls, "hpp": hpp, "text": read(hpp), "registered": False})
    return items


def render(item: dict) -> str:
    cls, reg, hpp = item["cls"], item["name"], item["hpp"]
    text = item["text"]
    impl = impl_file(cls, hpp)
    cpp = read(impl) if impl is not None else ""
    summary, sections, first_line = class_doc_comment(text, cls)
    ctor = ctor_of(text, cls)
    rw, ro = props_of(cpp, cls)
    acts = actions_of(cpp, cls)
    family = FAMILY.get(reg, "misc")
    rel_hpp = hpp.as_posix()
    rel_cpp = impl.as_posix() if impl is not None else ""
    refs = design_refs(text) + design_refs(cpp)

    out: list[str] = []
    out.append("---")
    out.append(f"title: {reg} · {FAMILY_TITLE.get(family, family)}")
    out.append(f"summary: {summary[:160] or f'{reg} 的构造、属性面、动作面与源码位置。'}")
    out.append("---")
    out.append("")
    out.append(f"# {reg}")
    out.append("")
    if summary:
        out.append(summary)
        out.append("")
    out.append("## 头与实现")
    out.append("")
    out.append(f"- 头：`{rel_hpp}`")
    out.append(f"- 实现：`{rel_cpp}`" if cpp else "- 实现：无独立 `.cpp`（定义在其他组件文件或 `element.hpp`）")
    out.append(f"- 注册名（XML / `ui.create` / 声明式按名建）：`{reg}`" if item["registered"]
               else "- **不进注册表**：XML / `ui.create` / 声明式按名建**不会生效**，由 C++ 直接 `new` 挂树（理由见头文件末尾）")
    out.append("")
    out.append("```cpp")
    out.append(f'#include "st/ui/components/{hpp.name}"')
    if ctor:
        out.append(f"// 构造：{ctor}")
        out.append(f"auto w = std::make_unique<st::ui::{cls}>();")
    else:
        out.append(f"auto w = std::make_unique<st::ui::{cls}>();  // 构造参数见头文件")
    out.append("```")
    out.append("")
    out.append("## 属性面（控制通道 `get` / `set`）")
    out.append("")
    if rw or ro:
        if rw:
            out.append("**可读写**：" + " · ".join(f"`{n}`" for n in rw))
        if ro:
            out.append("")
            out.append("**只读**（能 `get`，`set` 不生效）：" + " · ".join(f"`{n}`" for n in ro))
        out.append("")
        out.append("> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类"
                   "**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。")
    else:
        out.append("本组件**没有自己的属性面**——只能读写通用属性（`id`/`visible`/几何/样式）"
                   "与它自己的 C++ 接口。要断言内容，用 `tree` 的 `text`/`value` 字段或截图。")
    out.append("")
    out.append("## 动作面（控制通道 `invoke`）")
    out.append("")
    if acts:
        out.append(" · ".join(f"`{a}`" for a in acts))
        out.append("")
        out.append("> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，"
                   "与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。")
    else:
        out.append("本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。")
    out.append("")
    if sections:
        out.append("## 头注释里的逐条取舍（读源码前先看这里）")
        out.append("")
        for title, lineno in sections[:12]:
            out.append(f"- 「{title}」— `{rel_hpp}:{lineno}`")
        out.append("")
        out.append("> 这些是**为什么这样做**的记录（多半是踩过的坑）。改行为之前逐条读，"
                   "否则会重新踩一遍。")
        out.append("")
    if refs:
        out.append("## 设计章节")
        out.append("")
        out.append(" · ".join(f"`DESIGN.md` §{r}" for r in refs[:10]))
        out.append("")
        out.append("> 章节号是稳定接口（源码注释按号寻址），按号 grep 不要按内容找。")
        out.append("")
    out.append("## 更细")
    out.append("")
    out.append(f"- 族页（同类控件怎么选、组合套路）：`components/{family}.md`")
    out.append("- 属性/动作/通用属性三张表的口径：`overview/properties.md`")
    out.append("- 用控制通道驱动它跑一遍：`overview/control-protocol.md`")
    out.append("")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只核对不写入")
    args = ap.parse_args()

    items = registry_items()
    unknown = [i["name"] for i in items if i["name"] not in FAMILY]
    if unknown:
        print(f"[gen] 未分族的组件（请在 FAMILY 里登记）：{', '.join(unknown)}")
        return 2
    items = items + unregistered_items()

    drift: list[str] = []
    written = 0
    for item in items:
        family = FAMILY[item["name"]]
        target = OUT_DIR / family / f"{item['name']}.md"
        content = render(item)
        if args.check:
            if not target.exists() or read(target) != content:
                drift.append(str(target))
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8", newline="\n")
        written += 1

    # 清理：已不存在的组件留下的旧页
    alive = {(FAMILY[i["name"]], i["name"]) for i in items}
    stale: list[Path] = []
    for page in OUT_DIR.glob("*/*.md"):
        if (page.parent.name, page.stem) not in alive:
            stale.append(page)
    if args.check:
        for path in stale:
            drift.append(f"{path}（组件已不存在）")
        if drift:
            print("[gen] 与源码不一致（跑一次 tools/agent_docs_gen.py）：")
            for path in drift:
                print(f"  - {path}")
            return 1
        print(f"[gen] 一致：{len(items)} 个组件页")
        return 0
    for path in stale:
        path.unlink()
    print(f"[gen] 写入 {written} 页；清理旧页 {len(stale)} 个")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
