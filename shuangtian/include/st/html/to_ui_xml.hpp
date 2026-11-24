#pragma once

/// @file to_ui_xml.hpp
/// @brief **HTML → 霜天 XML 界面描述**的翻译层：只翻视图，不碰逻辑。
///
/// ## 它解决什么
///
/// 「把一份 HTML 变成霜天界面」此前的做法是**人工重写**：读一遍 HTML + CSS，
/// 再手敲一份 `ui/main.xml`。而 HTML 里 90% 的内容是**布局与文字**——那部分
/// 完全可以机器翻：翻出来的 XML 交给 `st::ui::build_from_xml` 就是真元素树，
/// 逻辑（点击做什么、数据从哪来）由后续用 JS / C++ 补。
///
/// 于是本层的契约只有一句：**HTML 的视图 → 霜天 XML 的视图**。
/// 它**不生成任何 JS**、不写事件绑定、不碰数据源（见下「不做的事」）。
///
/// ## 翻译取向：语义/结构保真（不是像素保真）
///
/// 目标不是「与浏览器逐像素一致」，而是「结构对、可读、可改、随窗口自适应」。
/// 这与霜天的布局模型一致：flex 排流 + 文本折行，天然随窗口变化；
/// 而像素保真需要绝对定位（`absolute` + 手算坐标），既不可读也不响应缩放。
///
/// | HTML | 霜天 XML | 说明 |
/// |---|---|---|
/// | `<div>` / `<section>` / `<p>` / `<body>` | `<Column>` | 块级 → 纵向容器 |
/// | 仅含行内内容的块 | `<Column><Text multiline="true">…</Text></Column>` | 文本先行合并 |
/// | `<h1>`…`<h6>` | `<Heading level="n">` | 语义标题组件 |
/// | `<span>` / `<b>` / `<strong>` / `<em>` | `<Text>`（字重/颜色从 CSS 落 `style`） | 行内不再切分 |
/// | `<a href>` | `<Link url="…">` | 跳转语义 |
/// | `<button>` | `<Button label="…">` | 动作语义 |
/// | `<input>` / `<textarea>` | `<Input>` / `<TextArea>` | 取值语义 |
/// | `<select>` / `<option>` | `<Select options="a|b|c" value="…">` | |
/// | `<ul>` / `<ol>` / `<li>` | `<Column>` + 带 `•`/`1.` 前缀的 `<Text>` | |
/// | `<table>` / `<tr>` / `<td>` | `<Table columns="…">`（有表头时）/ `<Row>` 网格 | |
/// | `<hr>` | `<Divider>` | |
/// | `<pre>` / `<code>` | `<Text multiline="true" monospace="true">` | 保留空白 |
/// | `<blockquote>` | `<Column style="…border-left…">` | 引用式左边框 |
/// | `<img>` | `<Avatar name="…">` 占位 + 报告 `unsupported_images` | 见「不做的事」 |
///
/// CSS → XML 有两条落点：
/// 1. **能映射到属性面的**（`width`/`padding`/`gap`/`font-size`/`color`/`radius`/
///    `background`/`border`…）→ 写成 XML 属性；
/// 2. 其余（含渐变、阴影、玻璃等霜天 `style` 串支持的）→ 汇成一条
///    `style="…"`（与 DSL/控制通道**同一份键表**，拼错会被 XML 建树报告抓出来）。
///
/// ## 不做的事（如实列全）
///
/// - **不生成逻辑**：不写 `on_click`、不写 JS、不绑事件（那是后续人工/模型的工作）。
/// - **不做像素保真**：忽略 `float` / `position` / `transform` / `flex` / `grid` 等
///   （`st::html` 解析层本就忽略它们）；百分比尺寸按基准宽换算成像素。
/// - **不取外部资源**：`<img src>` / `<link rel=stylesheet>` 不下载（离线、确定性）。
///   图片落成 `<Avatar>` 占位（首字），并记进报告。
/// - **不执行脚本**：`<script>` 内容丢弃（并在报告里计数）。
/// - **行内样式不逐段切分**：一个块里的 `<b>`/`<em>` 混排会被合并成一个 `Text`，
///   字重/颜色取该块的主样式（霜天 `Text` 是单样式单元，同一行内多字重需要
///   多个元素，而按 CSS 盒子切分会让排版与原文顺序对不上）。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/html/dom.hpp"

namespace st::html {

/// 转换选项。
struct ToUiXmlOptions {
  /// 根元素是否带 `grow="true"`。
  ///
  /// 默认**是**：XML 树挂在 `WindowFrame::content()` 下，根不带 grow 会让内容
  /// 贴顶、底部留白（`docs/xml-ui.md` 的「常见坑」第一条）。
  bool root_grow{true};
  /// 每个元素是否生成 `id`（`id="n1"` 这类稳定序号）。
  ///
  /// 默认**是**：没有 id 的元素只能按序号定位，自动化（控制通道 `find`/`get`）
  /// 与后续手写逻辑都要靠它。HTML 里本来有 `id` 的沿用原名。
  bool assign_ids{true};
  /// 缩进（空 = 不缩进，单行输出）。
  std::string indent{"  "};
  /// 文本里保留换行的块标签（`<pre>` 语义）。默认只认 `pre`/`textarea`。
  bool keep_pre_whitespace{true};
};

/// 转换报告：**如实报出丢了什么**（与 `XmlBuildReport` 同一取向——不静默）。
struct ToUiXmlReport {
  /// 一共产出多少元素（含容器）。
  std::size_t elements{0};
  /// HTML 标签 → 霜天组件 的命中统计（`tag → 次数`，便于核对"到底翻了什么"）。
  std::vector<std::pair<std::string, std::size_t>> tags{};
  /// 被忽略的 CSS 声明（去重；`属性: 值`）。这些**没有**落进 XML。
  ///
  /// 为何要报：HTML 里 `box-shadow` 满天飞，静默丢掉会让人以为"翻了但样式不对"，
  /// 而真相是那条声明从一开始就不受支持（霜天的 `style` 串支持阴影，只是
  /// CSS 的写法需要翻译——那属于未实现的映射，不是"不支持"）。
  std::vector<std::string> dropped_declarations{};
  /// 未映射的标签（内容仍会尽量保留，标签本身降级成 `<Column>` / `<Text>`）。
  std::vector<std::string> unmapped_tags{};
  /// 图片数量（落成 `<Avatar>` 占位；**像素内容不在 XML 里**）。
  std::size_t images{0};
  /// 被丢弃的 `<script>` 数量。
  std::size_t scripts{0};
  /// 诊断（空 = 一切在预期内）。
  std::vector<std::string> notes{};
};

/// 转换结果。
struct ToUiXmlResult {
  /// 生成的 XML 文本（根元素是单个 `<Column>`）。
  std::string xml{};
  /// 文档标题（`<title>`；没有为空）。宿主可用它当窗口标题。
  std::string title{};
  ToUiXmlReport report{};
};

/// 把 HTML 文本翻成霜天 XML。
///
/// 永不失败：畸形 HTML 由 `st::html::parse` 容错成树，翻不动的地方降级并进报告。
[[nodiscard]] auto to_ui_xml(std::string_view html, const ToUiXmlOptions& options = {})
    -> ToUiXmlResult;

/// 已解析的文档 → XML（要用自己的解析结果/做预处理时用）。
[[nodiscard]] auto to_ui_xml(const Document& document, const ToUiXmlOptions& options = {})
    -> ToUiXmlResult;

}  // namespace st::html
