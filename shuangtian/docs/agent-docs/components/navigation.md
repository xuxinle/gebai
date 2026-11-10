---
title: 导航与组织族 · 面包屑/标签/步骤/菜单
summary: 让用户在"哪一层、下一步去哪、有哪些入口"上有明确答案；按导航形态选，不要叠加两种同级导航。
---

# 导航与组织（navigation）

## 选型

| 用户的问题 | 用 | 不要 |
|---|---|---|
| 我在哪一层 | `Breadcrumb`（末段是当前位置、不可点） | 用 `Tabs` 表达层级 |
| 同一层的几个视图 | `Tabs`（切换、可关闭、可带新增钮） | 用 `Breadcrumb` 表达平级 |
| 分步流程走到第几步 | `Steps`（横/纵两向） | 用多个 `Tabs` |
| 数据分页 | `Pagination`（中段按 `sibling_count` 折叠） | 自己算页码文字 |
| 折叠分组（设置页/FAQ） | `Accordion`（单选/多选、受控展开） | 一列 `Tabs` |
| 一行操作入口 | `Toolbar`（横向/纵向、分组分隔、溢出折叠） | 一排散落的 `Button` |
| 顶部菜单 | `MenuBar` + `MenuPanel` | 自绘下拉 |
| 右键菜单 | `ContextMenu(anchor, items)` | 用 `SelectPanel` 冒充 |
| 键盘优先的命令入口 | `CommandPalette`（输入过滤 + 命令列表） | 让用户去翻菜单树 |
| 层级数据（文件树） | `Tree`（**扁平数组驱动**，不是元素树） | 递归塞 `Panel` |
| 底部状态信息 | `StatusBar`（左/右分段，段可带图标/色调/点击） | 堆 `Text` 手工对齐 |

## 组合套路

**IDE 形态**：`MenuBar`（顶） + `Tabs`（文档区） + `Breadcrumb`（路径） + `StatusBar`（底）
+ `CommandPalette`（`Ctrl+Shift+P`）。这五行就是 gbcode 的骨架。

**设置页**：左侧 `Tree`（分类） + 右侧 `ScrollView { Accordion }`。

## 坑

| 坑 | 说明 |
|---|---|
| `Tabs` 尾部钮要"先清后加" | 声明式里直接追加会出现重复/闪烁（实测 §4.2.17b） |
| `MenuBar` 一次点击触发两次 | 曾是真实缺陷；现在同一点击只派发一次，回归已钉住 |
| 标签条与分割线不该画焦点环 | §4.2.17a：聚焦不画环是**有意的**，别"修"回来 |
| `Tree` 不是元素树 | 它用扁平数组 `rows` + 自绘，子行不是 `Element`；按选择器找子行会失败，用 `rows` 属性面查 |
| `Breadcrumb` 末段可点会误导 | 当前位置默认不可点；`last_clickable` 只在"重新加载当前页"语义下打开 |

## 本族清单

`Accordion` · `Breadcrumb` · `Pagination` · `Steps` · `Tabs` · `Toolbar` · `StatusBar` ·
`MenuPanel` · `MenuBar` · `ContextMenu` · `CommandPalette` · `Tree`

- 逐个：`components/navigation/Accordion.md` · `components/navigation/Breadcrumb.md` · `components/navigation/Pagination.md`
- 逐个：`components/navigation/Steps.md` · `components/navigation/Tabs.md` · `components/navigation/Toolbar.md`
- 逐个：`components/navigation/StatusBar.md` · `components/navigation/MenuPanel.md` · `components/navigation/MenuBar.md`
- 逐个：`components/navigation/ContextMenu.md` · `components/navigation/CommandPalette.md` · `components/navigation/Tree.md`

## 更细

- 焦点与键盘导航约定：`DESIGN.md` §4.2.17（含 §4.2.17a–d）
- 侧栏常驻与隐藏保活：`DESIGN.md` §4.2.14
- 命令面板形态（VSCode `Ctrl+Shift+P`）：`include/st/ui/components/command_palette.hpp`
