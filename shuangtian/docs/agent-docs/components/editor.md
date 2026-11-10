---
title: 编辑器与文档视图族 · CodeEditor/Terminal/Diff
summary: 这一族最重：语法高亮、行装饰、三层坐标系、PTY 会话。四个"不知道就一定写错"的契约。
---

# 编辑器与文档视图（editor）

## 选型

| 要显示/编辑 | 用 | 关键点 |
|---|---|---|
| 代码（要高亮/行号/选择/撤销） | `CodeEditor` | `language` 决定高亮；`set_text` 会重置状态 |
| 差异（并列/内联） | `DiffView` | 三层坐标系不要混 |
| 三方合并 | `MergeView` | 冲突逐段取 ours/theirs |
| Markdown（含流式） | `MarkdownView` | `append_chunk` 流式追加，面向大模型输出 |
| HTML（基础 CSS 子集） | `HtmlView` | 无 JS、不联网、零依赖 |
| 终端 | `Terminal` | PTY 会话；无头下要真实 shell |
| 选文件/保存 | `FileDialog` | `places`/`filters` 由应用喂 |
| 代码补全弹窗 | `CompletionPopup` | 位置由调用方算 |

## 四个必知契约

**① `CodeEditor::set_text` 是"换了一份文档"**：光标归零、滚动归零、撤销栈清空。
重排/重算路径用它 = 每次当新文档。要"保位置"用 `replace_text_keep_state`（按行列重定位、保滚动、保撤销栈）。

**② 三层坐标系（`DiffView`）**：内容坐标 / 行坐标 / 屏幕坐标——混一次就全错。
行装饰按行号索引，绘制按内容偏移，命中按屏幕位置；转换只在明确的边界函数里做。

**③ 排版宽必须等于控件宽**：文本组件的排版宽度若不是控件宽度，"选中背景错位/不随窗口适应"
（§4.2.17h）。改宽度要重排版，不是只改 `bounds`。

**④ 终端的多会话与 alt screen**：`Terminal` 用会话表 + 活动会话；`alt_screen` 要显式切换，
否则全屏程序（vim/less）退出后内容错乱。

## 组合套路

**IDE 编辑区**：`Tabs { CodeEditor } + CompletionPopup + StatusBar(光标行列)`。
光标位置从 `cursor`/`line`/`column` 属性面读，不要从像素反推。

**流式生成面板**：`MarkdownView` + `append_chunk`，末尾 `stick_to_bottom` 让视口跟随。

## 坑

| 坑 | 说明 |
|---|---|
| 每帧 `set_text` | 用户每敲一个字光标跳行首、撤销历史消失 |
| 用 `set_highlight` 语言名拼错 | 静默退化为纯文本（没有高亮但也不报错）；断言 `highlight` 属性面 |
| 在编辑器里自绘选中背景 | 统一原语在 `text_selection`（§4.2.17g/§4.2.17i），两套几何量极易混 |
| `DiffView` 尺寸改后不重排 | 行装饰会错位；改尺寸必须触发重新排版 |
| 终端命令直接用 `system()` | 要走 PTY + `process` 封装；否则无头/Windows 下行为不同 |

## 本族清单

`CodeEditor` · `DiffView` · `MergeView` · `MarkdownView` · `HtmlView` · `Terminal` · `FileDialog` · `CompletionPopup`

- 逐个：`components/editor/CodeEditor.md` · `components/editor/DiffView.md` · `components/editor/MergeView.md`
- 逐个：`components/editor/MarkdownView.md` · `components/editor/HtmlView.md` · `components/editor/Terminal.md`
- 逐个：`components/editor/FileDialog.md` · `components/editor/CompletionPopup.md`

## 更细

- 语法高亮与编辑器设计：`DESIGN.md` §4.2.10
- 结构化差异与并列视图（三层坐标系、行装饰、配置面/内容面分离）：`DESIGN.md` §4.2.11
- 文本选中统一原语与两套几何量：`DESIGN.md` §4.2.17g · §4.2.17i
- 终端修复轮（光标错位/回显/多会话串台）：`docs/TERMINAL_FIXES_20261008.md`
- 流式 Markdown（面向大模型）：`DESIGN.md` §4.4
