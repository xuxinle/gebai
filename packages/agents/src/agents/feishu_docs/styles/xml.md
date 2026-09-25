# XML 排版语法（import_xml）

类 HTML 标签、纵向块级文档流：顶层块按顺序纵向排列，容器块内可嵌套子块。
属性必须写 `name="value"`（引号推荐，无引号也接受）。飞书正文可用宽度约 820px。
文本转义：`<` → `&lt;`、`>` → `&gt;`、`&` → `&amp;`；**标签本身不要转义**。

## 基础块标签

| XML | 落地效果 |
|---|---|
| `<title>文档标题</title>` | 文档标题（整篇唯一，放最前；与标题相同的首个 H1 自动去重） |
| `<h1>`~`<h9>`，可加 `seq="auto"` | 标题 1~9 级；`seq="auto"` 自动编号（1 / 1.1 / 1.1.1），标题文本不要手写序号 |
| `<p align="center">正文</p>` | 段落；`align` 可选 `left`/`center`/`right` |
| `<ul><li>项<ul><li>子项</li></ul></li></ul>` | 无序列表（子列表写在 `<li>` 内，嵌套 ≤ 2 层） |
| `<ol><li>项</li></ol>`、`<ol seq="3">` | 有序列表；默认自动编号，`seq` 指定起始序号 |
| `<checkbox done="true">事项</checkbox>` | 待办（勾选态） |
| `<blockquote>引用</blockquote>` | 引用块（平台不支持子块，内部内容按软换行合并） |
| `<hr/>` | 分割线 |
| `<br/>` | 行内换行 / 单元格内换行（连续两个 = 单元格内新段落） |
| `<pre lang="go" caption="示例"><code>…</code></pre>` | 代码块（代码必须放 `<code>` 内；`lang` 标语言、`caption` 生成题注段落） |
| `<latex>E=mc^2</latex>` | 落地为文本（飞书 equation 块不可经 API 创建，公式需手动插入公式块） |

## 行内标签

`<b>` `<strong>` 加粗 · `<em>` `<i>` 斜体 · `<u>` 下划线 · `<del>` `<s>` 删除线 · `<code>` 行内代码 ·
`<a href="URL">标题</a>` 链接（`type="url-preview"` 预览卡片暂不支持，按普通链接落地）·
`<span text-color="red" background-color="light-blue">…</span>` 文字色/底色·
`<cite type="user" user-id="ou_xxx"/>` **@人**（须真实 open_id；自闭合，不写内容）。

## 容器与富块

| XML | 落地效果 |
|---|---|
| `<callout emoji="bulb" background-color="light-blue" border-color="blue" text-color="blue"><p>内容</p></callout>` | 高亮块；内部只支持段落/列表/待办（表格、图片、代码、分栏会被移出并提示）；**至少一个子块**，空内容自动补空段落 |
| `<grid><column width-ratio="0.5"><p>左栏</p></column><column width-ratio="0.5"><p>右栏</p></column></grid>` | 分栏（2~5 列；`width-ratio` 为相对比例，缺省 1；每列至少一个子块，空列自动补空段落） |
| `<table><colgroup><col width="180"/><col width="550"/></colgroup><thead><tr><th>表头</th></tr></thead><tbody><tr><td>内容</td></tr></tbody></table>` | 表格；`<colgroup>` 定列宽（数量须等于列数，否则回落内容自适应）；`<thead>` 或 `<th>` 判表头行；单元格内可用 `<br>` 换行、行内加粗/代码/链接 |
| `<img path="@./shot.png" caption="界面截图"/>` | 图片（`path` 本地路径、`href` 网络地址；`caption` 生成斜体题注段落；**宽高设置不支持**——实测 image.size 只读，写了会提示） |
| `<whiteboard type="mermaid">graph TD; A-->B</whiteboard>` | 图示：`type` 支持 `mermaid`/`plantuml`/`d2`/`echarts`（服务端渲染为图片插入），或 `path="@./a.mmd"` 引用本地源码文件（按扩展名自判格式） |
| `<whiteboard type="svg" path="@./pic.svg"/>`（或内联 `<svg>…</svg>`） | SVG 直接作为图片插入（保真显示；飞书画板无法从 SVG 创建，因此不可编辑） |

## 颜色

- 合法色相：`red orange yellow green blue purple gray`；
- 高亮块背景支持 `light-*`（浅色，默认）与 `medium-*`（强提醒）；高亮块边框/文字色、行内文字色只用基础色相（`medium-*` 会回落基础色并提示）；
- 颜色表达语义并全文一致，默认中性，不整段染色、不整表铺色。

## 暂不支持（写了会提示并降级，不会静默丢失）

`<bookmark>` `<button>` `<time>` `<task>` `<sheet>` `<source>`（附件）`<html5-block>` `<okr>`；
`<cite type="doc">`（**@文档**：实测飞书返回 1770038，改用普通链接）；
表格单元格的 `background-color`/`colspan`/`rowspan`、图片宽高（image.size 只读）、公式块（equation 块不可经 API 创建）。

## 与 Markdown 的分工

| 场景 | 用什么 |
|---|---|
| 整篇创作、需要自动编号/分栏/高亮块配色/表格列宽/题注 | `import_xml` |
| 快速追加、内容简单、已有 Markdown 草稿 | `import_markdown`（表达能力是 XML 的子集） |
| 局部改块 | `update_block`（整块文本）/ `replace_text`（跨块查找替换，先用 dry_run 预览）/ `add_blocks` / `delete_blocks`（块级写法见 add_blocks 工具描述） |
| 看完就改（拿 block_id） | `get_doc_blocks detail=compact`：一行一块 `{缩进}{类型} [block_id] {摘要}`，表格只给行列数不展开单元格——轻量读完且每行可直接拿去改；需样式/原始字段时用 `detail=full` |
| 定位命中处上下文 | `find_blocks` 传 `context_before`/`context_after`（`▶` 命中行、`·` 上下文行） |
| 写入前预检 | `import_xml` 传 `dry_run=true`：只解析并出画像（块数/字数/类型分布）+ 图片与图表预检，**零写入** |
