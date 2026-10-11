/// `st::xml` 的解析契约测试。
///
/// 重点在**容错路径**：这是资源装载通道，畸形输入不该让应用起不来，但也不该静默
/// 吞成"看起来正常"的空树。所以每条容错都配一个"仍然解析出了什么"的断言。

#include "st/xml/xml.hpp"

#include <string>
#include <vector>

#include "st/test/test.hpp"

namespace {

using st::xml::Document;
using st::xml::Node;

auto parse_ok(std::string_view source) -> Document {
  st::xml::Error error;
  auto doc = st::xml::parse(source, &error);
  if (!doc.has_value()) {
    ST_FAIL("解析失败：" + error.message);
    return Document{};
  }
  return std::move(*doc);
}

/// 收集某层直接子元素的名字（断言树形）。
///
/// 返回**拼接后的字符串**而不是 vector：`ST_CHECK_EQ` 要 `std::format` 两侧，
/// 而 `vector<string>` 没有 formatter——用 `A|B` 形式既可比也可读。
auto child_names(const Node& node) -> std::string {
  std::string out;
  for (const auto& child : node.children) {
    if (!out.empty()) out.push_back('|');
    out += child.name;
  }
  return out;
}

}  // namespace

ST_TEST(xml_parses_elements_attributes_and_text) {
  const Document doc = parse_ok(R"(<?xml version="1.0"?>
<Card title="设置" gap="8">
  <Text color="#ff0000">你好</Text>
  <Button id="ok">确定</Button>
</Card>)");
  ST_CHECK_EQ(doc.root.name, std::string("Card"));
  ST_CHECK_EQ(*doc.root.attr("title"), std::string_view("设置"));
  ST_CHECK_EQ(doc.root.attr_number("gap").value_or(-1.0), 8.0);
  ST_REQUIRE(doc.root.children.size() == 2);
  ST_CHECK_EQ(doc.root.children[0].name, std::string("Text"));
  ST_CHECK_EQ(doc.root.children[0].text, std::string("你好"));
  ST_CHECK_EQ(doc.root.children[1].text, std::string("确定"));
  // 子元素之间的缩进空白不该跑进父元素的文本（折叠规则）。
  ST_CHECK(doc.root.text.empty());
}

ST_TEST(xml_single_root_needs_no_child_index) {
  // 只有一个顶层元素时，`root` 就是它——调用方不必写"取第一个孩子"。
  const Document doc = parse_ok("<Root><A/></Root>");
  ST_CHECK_EQ(doc.root.name, std::string("Root"));
  ST_CHECK_EQ(child_names(doc.root), std::string("A"));
}

ST_TEST(xml_multiple_top_level_nodes_collect_under_empty_root) {
  // 资源文件里常见「注释 + 多个并列块」；不该因此解析失败。
  const Document doc = parse_ok("<!-- 说明 -->\n<A/>\n<B/>");
  ST_CHECK(doc.root.name.empty());
  ST_CHECK_EQ(child_names(doc.root), std::string("A|B"));
}

ST_TEST(xml_quoted_angle_bracket_is_not_a_tag_end) {
  // 实测最容易踩的边界：属性值里的 `>`。
  const Document doc = parse_ok(R"(<A label="a > b" other='c < d'/>)");
  ST_CHECK_EQ(*doc.root.attr("label"), std::string_view("a > b"));
  ST_CHECK_EQ(*doc.root.attr("other"), std::string_view("c < d"));
}

ST_TEST(xml_entities_decode_and_unknown_ones_survive) {
  const Document doc = parse_ok(
      R"(<A t="&lt;tag&gt; &amp; &quot;q&quot;" u="&#65;&#x42;">a&amp;b &#67;</A>)");
  ST_CHECK_EQ(*doc.root.attr("t"), std::string_view("<tag> & \"q\""));
  ST_CHECK_EQ(*doc.root.attr("u"), std::string_view("AB"));
  ST_CHECK_EQ(doc.root.text, std::string("a&b C"));
  // 认不出的实体**原样保留**（不猜语义）。
  const Document other = parse_ok("<A>x &nbsp; y</A>");
  ST_CHECK_EQ(other.root.text, std::string("x &nbsp; y"));
}

ST_TEST(xml_self_closing_and_bare_boolean_attributes) {
  const Document doc = parse_ok("<A grow visible=\"false\" count=3/>");
  ST_CHECK(doc.root.attr_bool("grow") && "裸属性 = 真");
  ST_CHECK(!doc.root.attr_bool("visible", true));
  ST_CHECK_EQ(doc.root.attr_number("count").value_or(0.0), 3.0);
  ST_CHECK_EQ(doc.root.attr_or("missing", "fallback"), std::string_view("fallback"));
}

ST_TEST(xml_cdata_is_literal_and_not_collapsed) {
  // CDATA 的字面语义：内部空白/实体**一字不改**。
  const Document doc = parse_ok("<Script><![CDATA[a  b & c\nd]]></Script>");
  ST_CHECK_EQ(doc.root.text, std::string("a  b & c\nd"));
}

ST_TEST(xml_unclosed_tags_are_recovered_not_fatal) {
  // 容错：漏了 `</B>`。不报致命错，且 **A 仍然在树里**。
  st::xml::Error error;
  auto doc = st::xml::parse("<A><B><C/></A>", &error);
  ST_REQUIRE(doc.has_value());
  ST_CHECK_EQ(doc->root.name, std::string("A"));
  ST_REQUIRE(doc->root.children.size() == 1);
  ST_CHECK_EQ(doc->root.children[0].name, std::string("B"));
  ST_CHECK_EQ(child_names(doc->root.children[0]), std::string("C"));
  ST_CHECK(!error.message.empty());   // 仍然给出诊断
}

ST_TEST(xml_stray_close_tag_is_ignored_and_reported) {
  st::xml::Error error;
  auto doc = st::xml::parse("<A/></B><C/>", &error);
  ST_REQUIRE(doc.has_value());
  ST_CHECK(!error.message.empty());
  // 三个顶层元素都在（含被忽略的 </B> 之后的 C）。
  ST_CHECK(doc->root.name.empty());
  ST_CHECK_EQ(child_names(doc->root), std::string("A|C"));
}

ST_TEST(xml_comments_doctype_and_pi_are_skipped) {
  const Document doc = parse_ok(
      "<!DOCTYPE x [<!ENTITY y \"z\">]>\n<?pi here?>\n<!-- c1 --><A><!-- c2 --><B/></A>");
  ST_CHECK_EQ(doc.root.name, std::string("A"));
  ST_CHECK_EQ(child_names(doc.root), std::string("B"));
}

ST_TEST(xml_line_numbers_are_reported) {
  st::xml::Error error;
  (void)st::xml::parse("<A>\n<B>\n<C/></A>", &error);
  ST_CHECK(error.line >= 3);   // 未闭合那条诊断应指到最后一行附近

  const Document doc = parse_ok("<A>\n  <B/>\n</A>");
  ST_REQUIRE(doc.root.children.size() == 1);
  ST_CHECK(doc.root.children[0].line == 2u);   // B 在第 2 行
}

ST_TEST(xml_deep_nesting_is_bounded_not_crashing) {
  std::string source;
  for (int index = 0; index < 400; ++index) source += "<N>";
  st::xml::Error error;
  auto doc = st::xml::parse(source, &error);
  // 只要求"不崩、有诊断"，不要求具体深度（护栏数字是内部约定）。
  ST_CHECK(!error.message.empty());
}

ST_TEST(xml_empty_input_is_nullopt_with_reason) {
  st::xml::Error error;
  ST_CHECK(!st::xml::parse("", &error).has_value());
  ST_CHECK(!error.message.empty());
  ST_CHECK(!st::xml::parse("   \n  ", &error).has_value());
}

ST_TEST(xml_children_named_filters_by_name) {
  const Document doc = parse_ok("<Root><Item id=\"1\"/><Other/><Item id=\"2\"/></Root>");
  const auto items = doc.root.children_named("Item");
  ST_REQUIRE(items.size() == 2);
  ST_CHECK(*items[1]->attr("id") == std::string_view("2"));
  ST_CHECK(doc.root.child("Missing") == nullptr);
  ST_CHECK(!doc.root.empty());
  ST_CHECK(parse_ok("<A/>").root.empty());
}

ST_TEST(xml_escape_round_trips_through_parse) {
  const std::string original = R"(<a & b "c" 'd'>)";
  const std::string encoded = st::xml::escape(original);
  const Document doc = parse_ok("<A t=\"" + encoded + "\">" + encoded + "</A>");
  ST_CHECK(*doc.root.attr("t") == std::string_view(original));
  ST_CHECK(doc.root.text == original);
}
