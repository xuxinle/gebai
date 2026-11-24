// HTML → 霜天 XML 的转换测试。
//
// ## 判据为什么这样选
//
// 生成本身"看着对"很容易（拼串嘛），所以本文件不检查字符串片段好不好看，
// 只钉三类**会真的坏掉下游**的事：
//
// 1. **语义映射**：`<h1>` 必须是 `Heading level="1"`（而不是"字号大的 Text"）——
//    否则后续逻辑按语义找元素（`find Heading`）就找不到。
// 2. **样式真的落进去**：CSS 的 `color` 必须变成那个 `Text` 自己的 `text_color`。
//    霜天没有 CSS 式继承，父容器的颜色**不会下沉**；漏复制就是"整页全主题默认色"。
// 3. **生成的 XML 能被 `build_from_xml` 的严格模式吃下**——这是最硬的一条：
//    它同时验证属性名在白名单里、值能被接受、结构合法。生成器一旦写出
//    `padding_top` 这种不存在的名字，这条立刻红。
//
// 第 3 条是"属性名对不对"的唯一可靠判据：拿生成结果去真建树，让框架自己说
// 认不认。手工维护一份"允许的属性名清单"必然与源码漂移。

#include <memory>
#include <string>
#include <string_view>

#include "st/html/to_ui_xml.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"
#include "st/ui/xml_ui.hpp"

namespace {

using namespace st::html;

/// HTML → XML → 真元素树（严格模式）。返回 `nullptr` = 生成的 XML 不合法。
///
/// 严格模式（`XmlBuildOptions{.strict = true}`）在报告不干净时返回 `nullptr`，
/// 于是"生成了框架不认识的东西"会在这里变成明确的失败，而不是建出一棵缺元素的树。
struct Built {
  std::string xml{};
  ToUiXmlReport report{};
  st::ui::XmlBuildReport build{};
  std::unique_ptr<st::ui::Element> tree{};

  explicit Built(std::string_view source) {
    ToUiXmlResult result = to_ui_xml(source);
    xml = std::move(result.xml);
    report = std::move(result.report);
    st::ui::XmlBuildOptions options;
    options.strict = true;
    tree = st::ui::build_from_xml(xml, &build, options);
  }
};

/// 在树里按 id 找元素（生成器给每个元素都编了 `n1`/`n2` 这类 id）。
auto find_type(const st::ui::Element& element, std::string_view type) -> const st::ui::Element* {
  if (element.type() == type) return &element;
  for (const auto& child : element.children()) {
    if (const auto* found = find_type(*child, type); found != nullptr) return found;
  }
  return nullptr;
}

auto count_type(const st::ui::Element& element, std::string_view type) -> int {
  int total = element.type() == type ? 1 : 0;
  for (const auto& child : element.children()) total += count_type(*child, type);
  return total;
}

/// 数「行容器」。
///
/// `<Row>` / `<Column>` 在霜天里**都是 `Panel`**（只差 `direction`）——
/// 按类型名 `Row` 数永远是 0。
auto count_rows(const st::ui::Element& element) -> int {
  const bool row = element.type() == "Panel" &&
                   element.get_property("direction").value_or("") == "row";
  int total = row ? 1 : 0;
  for (const auto& child : element.children()) total += count_rows(*child);
  return total;
}

}  // namespace

/// 最基本的语义映射：`<div>` → `Column`，`<p>` → `Column` 里一个多行 `Text`。
ST_TEST(html_to_xml_maps_basic_structure) {
  const Built built("<div><p>第一段</p><p>第二段</p></div>");
  ST_REQUIRE(built.tree != nullptr);           // 严格模式：生成的 XML 必须干净
  ST_CHECK(built.build.unknown_tags.empty());
  ST_CHECK(built.build.unknown_attributes.empty());
  // 注意：`<Column>` 在霜天里就是 `Panel`（方向不同）——`type()` 报的是 `Panel`。
  // 判"它是不是纵向容器"要读 `direction`，不能读类型名。
  ST_CHECK_EQ(std::string{built.tree->type()}, std::string{"Panel"});
  ST_CHECK_EQ(built.tree->get_property("direction").value_or(""), std::string{"column"});
  // 根 Column 下是 section Column，里面两个 p 各自的 Column + Text。
  ST_CHECK(count_type(*built.tree, "Text") == 2);
}

/// 标题必须是 `Heading` + `level`（语义，而不是"字号大的 Text"）。
///
/// 为什么这条重要：`Heading` 的字号由 `level` 走主题 token，若把它降级成
/// 带 `text_size` 的 `Text`，换主题时标题就不再跟随字号阶梯（且逻辑层按
/// `Heading` 找标题会落空）。
ST_TEST(html_to_xml_maps_headings_with_level) {
  const Built built("<h1>大标题</h1><h3>小标题</h3>");
  ST_REQUIRE(built.tree != nullptr);
  const auto* h1 = find_type(*built.tree, "Heading");
  ST_REQUIRE(h1 != nullptr);
  ST_CHECK_EQ(h1->get_property("level").value_or(""), std::string{"1"});
  ST_CHECK_EQ(h1->semantics_text(), std::string{"大标题"});
  // 两个标题都在（第二个是 h3）。
  ST_CHECK(count_type(*built.tree, "Heading") == 2);
}

/// CSS 的颜色/字号必须落到**每个 Text 自己身上**（霜天没有 CSS 继承）。
///
/// 若漏复制：界面不报错、建树报告干净，但整页都是主题默认色——那种缺陷
/// 只能靠肉眼发现。这条把它变成断言。
ST_TEST(html_to_xml_copies_text_style_onto_each_text) {
  const Built built(
      "<style>p { color: #ff0000; font-size: 22px; }</style>"
      "<p>红字</p>");
  ST_REQUIRE(built.tree != nullptr);
  const auto* text = find_type(*built.tree, "Text");
  ST_REQUIRE(text != nullptr);
  // 颜色：读回的是生效值（`#ff0000`）。
  const std::string color = text->get_property("color").value_or("");
  ST_CHECK(color.find("ff0000") != std::string::npos || color.find("FF0000") != std::string::npos);
  // 字号：22。
  //
  // ⚠ **必须过一遍真实上屏链路再断言**（`UiRoot::set_content` + `layout(true)`）：
  // 刚建完树时 `Style::font_size` 就已经是 22（写属性时直接落的），
  // 而真正的失败形态是“主题回放把它盖回 `font_base`”。不过这一遍就等于
  // 拿写入值当结果——实测过：把生成器的 `text_size` 改回 `font_size`
  // （那条通道会被 `Text::apply_theme` 盖回）后，**这条断言依旧绿**。
  st::ui::UiRoot root;
  auto owned = st::ui::build_from_xml(built.xml);
  ST_REQUIRE(owned != nullptr);
  root.set_content(std::move(owned));
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  root.layout(true);   // 主题下发（真实上屏走的就是这条）
  const auto* live = find_type(*root.content(), "Text");
  ST_REQUIRE(live != nullptr);
  const float effective = live->style().font_size;
  ST_CHECK_NEAR(effective, 22.0f, 0.05f);
  ST_CHECK(effective > root.theme().metrics().font_base + 3.0f);
}

/// 布局属性：块级元素的 `padding` / `background` 落成 XML 属性。
ST_TEST(html_to_xml_maps_box_style) {
  const Built built(
      "<style>.card { padding: 12px; background: #eeeeee; border-radius: 8px; }</style>"
      "<div class=\"card\">内容</div>");
  ST_REQUIRE(built.tree != nullptr);
  ST_CHECK(built.build.unknown_attributes.empty());
  const st::ui::Style& style = built.tree->children().empty() ? built.tree->style()
                                                             : built.tree->children().front()->style();
  (void)style;
  // 生成的 XML 文本里必须出现这三条（读回路径依赖元素类型，这里直接看产物文本）。
  ST_CHECK(built.xml.find("padding=\"12\"") != std::string::npos);
  ST_CHECK(built.xml.find("#eeeeee") != std::string::npos);
  ST_CHECK(built.xml.find("radius=\"8\"") != std::string::npos);
}

/// 交互语义：`<a>` → `Link`、`<button>` → `Button`、`<input>` → `Input`。
ST_TEST(html_to_xml_maps_interactive_semantics) {
  const Built built(
      "<a href=\"https://example.com\">去这里</a>"
      "<button>确定</button>"
      "<input placeholder=\"搜索\" type=\"text\">");
  ST_REQUIRE(built.tree != nullptr);
  const auto* link = find_type(*built.tree, "Link");
  ST_REQUIRE(link != nullptr);
  ST_CHECK_EQ(link->get_property("url").value_or(""), std::string{"https://example.com"});
  ST_CHECK_EQ(link->get_property("external").value_or(""), std::string{"true"});

  const auto* button = find_type(*built.tree, "Button");
  ST_REQUIRE(button != nullptr);
  ST_CHECK_EQ(button->semantics_text(), std::string{"确定"});

  const auto* input = find_type(*built.tree, "Input");
  ST_REQUIRE(input != nullptr);
  ST_CHECK_EQ(input->get_property("placeholder").value_or(""), std::string{"搜索"});
}

/// 列表：每项一个 `Row`（项目符号 + 内容），有序列表用序号。
ST_TEST(html_to_xml_maps_lists) {
  const Built built("<ul><li>甲</li><li>乙</li></ul><ol><li>丙</li></ol>");
  ST_REQUIRE(built.tree != nullptr);
  // 无序两条 + 有序一条 = 三个行容器。`Row` 也是 `Panel`（`direction="row"`），
  // 所以按方向数，而不是按类型名。
  ST_CHECK(count_rows(*built.tree) == 3);
  ST_CHECK(built.xml.find("•") != std::string::npos);
  ST_CHECK(built.xml.find("1.") != std::string::npos);
}

/// 行内标签**不切碎**：`<b>`/`<em>` 合并进同一个 `Text`，文本顺序不变。
///
/// 为什么不做逐段切分：霜天 `Text` 是单样式单元，按 CSS 盒子切开会让
/// "同一句话"变成多个 Text，折行时每段各自独占一行（排版彻底崩）。
ST_TEST(html_to_xml_merges_inline_runs) {
  const Built built("<p>前缀 <b>加粗</b> 中间 <em>斜体</em> 结尾</p>");
  ST_REQUIRE(built.tree != nullptr);
  ST_CHECK(count_type(*built.tree, "Text") == 1);
  const auto* text = find_type(*built.tree, "Text");
  ST_REQUIRE(text != nullptr);
  const std::string content = text->semantics_text();
  // 顺序与完整性：三段文字都在，且按原顺序。
  const std::size_t bold = content.find("加粗");
  const std::size_t italic = content.find("斜体");
  const std::size_t tail = content.find("结尾");
  ST_CHECK(content.find("前缀") == 0);
  ST_REQUIRE(bold != std::string::npos);
  ST_REQUIRE(italic != std::string::npos);
  ST_REQUIRE(tail != std::string::npos);
  ST_CHECK(bold < italic);
  ST_CHECK(italic < tail);
}

/// `<pre>` 保留空白与换行（`multiline` + `monospace`）。
///
/// 这条摔过两次：① 生成器写普通文本会被 `st::xml` 的空白折叠吃掉换行；
/// ② `monospace` 只能写不能读（`get_property` 没有它）——所以断言要看生效样式
/// （`style().monospace`），不看属性面。
ST_TEST(html_to_xml_keeps_preformatted) {
  const Built built("<pre>第一行\n    缩进四格\n第三行</pre>");
  ST_REQUIRE(built.tree != nullptr);
  const auto* text = find_type(*built.tree, "Text");
  ST_REQUIRE(text != nullptr);
  const std::string content = text->semantics_text();
  ST_CHECK(content.find('\n') != std::string::npos);
  ST_CHECK(content.find("    缩进四格") != std::string::npos);   // 行首缩进是内容
  ST_CHECK(text->style().monospace);
}

/// 报告：图片、脚本、未映射标签**如实报出**，不静默。
ST_TEST(html_to_xml_reports_losses) {
  const ToUiXmlResult result = to_ui_xml(
      "<script>var x=1;</script>"
      "<img src=\"a.png\" alt=\"图\">"
      "<blink>闪烁</blink>"
      "<p>正文</p>");
  ST_CHECK_EQ(result.report.scripts, std::size_t{1});
  ST_CHECK_EQ(result.report.images, std::size_t{1});
  // 未映射标签进报告（`blink` 不在已知块级表里）。
  bool blink_reported = false;
  for (const std::string& tag : result.report.unmapped_tags) {
    if (tag == "blink") blink_reported = true;
  }
  ST_CHECK(blink_reported);
  // 但**内容不丢**：`闪烁` 仍应在生成的 XML 里。
  ST_CHECK(result.xml.find("闪烁") != std::string::npos);
  // 图片的报告项必须提到"占位"（告诉调用方像素不在这里）。
  bool image_noted = false;
  for (const std::string& item : result.report.dropped_declarations) {
    if (item.find("占位") != std::string::npos) image_noted = true;
  }
  ST_CHECK(image_noted);
}

/// 文本转义：内容里的 `<`/`&`/引号不能破坏 XML 结构。
ST_TEST(html_to_xml_escapes_text) {
  const ToUiXmlResult result = to_ui_xml("<p>a &lt; b &amp;&amp; c &gt; d</p>");
  // 生成的 XML 里 `&` 必须是实体（否则建树时会被当实体开头）。
  ST_CHECK(result.xml.find("&lt;") != std::string::npos);
  ST_CHECK(result.xml.find("&amp;") != std::string::npos);
  // 真建一次树，确认解析回来仍是原文。
  auto tree = st::ui::build_from_xml(result.xml);
  ST_REQUIRE(tree != nullptr);
  const auto* text = find_type(*tree, "Text");
  ST_REQUIRE(text != nullptr);
  ST_CHECK_EQ(text->semantics_text(), std::string{"a < b && c > d"});
}

/// 根元素必须 `grow="true"`（否则挂在窗口内容区里会贴顶、底部留白）。
ST_TEST(html_to_xml_root_grows) {
  const ToUiXmlResult result = to_ui_xml("<p>x</p>");
  ST_CHECK(result.xml.find("grow=\"true\"") != std::string::npos);
}

/// HTML 里原有的 `id` 必须沿用（逻辑层要按它绑事件）。
ST_TEST(html_to_xml_keeps_html_ids) {
  const Built built("<div id=\"panel\"><button id=\"save\">保存</button></div>");
  ST_REQUIRE(built.tree != nullptr);
  auto root = st::ui::UiRoot{};
  auto owned = st::ui::build_from_xml(built.xml);
  ST_REQUIRE(owned != nullptr);
  root.set_content(std::move(owned));
  ST_CHECK(root.find("panel") != nullptr);
  ST_CHECK(root.find("save") != nullptr);
}
