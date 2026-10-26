/// `st::ui::build_from_xml` 的契约测试。
///
/// 重点三条（都是这一层存在的理由）：
/// ① 递归建树：容器/子元素/文本/属性都落到**同一种元素树**上（之后照样吃选择器、
///    控制通道、脚本）；
/// ② 属性走 `ui::apply_properties`（**与协议 `set` 同一条路**，不另立第二套属性名）；
/// ③ 认不出的标签/属性**如实报告**（静默会让"少了一个按钮"无从排查）。

#include "st/core/json.hpp"
#include "st/core/json.hpp"
#include "st/ui/xml_ui.hpp"

#include <format>
#include <string>

#include "st/core/print.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"
#include "st/test/test.hpp"

namespace {

using st::ui::Element;
using st::ui::UiRoot;

/// 把报告打成一行（**只在失败时**用：测试输出里看得到根因，不必另建探针）。
///
/// 为何要它：断言只说“report 不干净”，而排查要知道**具体哪一条**属性没落上、
/// 哪一行。把报告原样打出来比再猜一轮便宜得多。
auto describe(const st::ui::XmlBuildReport& report) -> std::string {
  std::string out = std::format("elements={} clean={}", report.elements, report.clean());
  for (const auto& tag : report.unknown_tags) out += std::format(" | tag:<{}>", tag);
  for (const auto& attr : report.unknown_attributes) out += std::format(" | attr:{}", attr);
  if (!report.parse_warning.empty()) out += std::format(" | parse:{}", report.parse_warning);
  return out;
}

auto stat_of(const Element& element, const char* name) -> std::string {
  auto* mutable_element = const_cast<Element*>(&element);
  return mutable_element->get_property(name).value_or("");
}

/// 在整棵子树里按 id 找元素（`Element` 自身没有 find）。
auto find_by_id(Element& root, std::string_view id) -> Element* {
  if (root.id() == id) return &root;
  for (std::size_t index = 0; index < root.child_count(); ++index) {
    if (Element* hit = find_by_id(*root.child_at(index), id); hit != nullptr) return hit;
  }
  return nullptr;
}

}  // namespace

ST_TEST(xml_ui_builds_nested_tree_with_text_and_attributes) {
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(R"(
    <Column gap="8" padding="12" id="page">
      <Text id="title" size="18">设置</Text>
      <Row gap="6" id="actions">
        <Button id="ok" variant="primary">确定</Button>
        <Button id="cancel">取消</Button>
      </Row>
    </Column>)", &report);
  ST_REQUIRE(tree != nullptr);
  if (!report.clean()) st::print("[xml-ui] 报告：{}\n", describe(report));
  ST_CHECK(report.clean());
  ST_CHECK_EQ(tree->id(), std::string("page"));
  ST_CHECK_EQ(tree->child_count(), std::size_t{2});
  // 文本落到 Text 上（走属性面，不是另立旁路）。
  Element* title = find_by_id(*tree, "title");
  ST_REQUIRE(title != nullptr);
  ST_CHECK_EQ(stat_of(*title, "text"), std::string("设置"));
  // 嵌套两层：Row 里两个 Button。
  Element* actions = find_by_id(*tree, "actions");
  ST_REQUIRE(actions != nullptr);
  ST_CHECK_EQ(actions->child_count(), std::size_t{2});
  ST_CHECK(actions->style().direction == st::ui::FlexDirection::Row);
  ST_CHECK_EQ(stat_of(*find_by_id(*tree, "ok"), "label"), std::string("确定"));
}

ST_TEST(xml_ui_style_properties_land_on_style) {
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(
      R"(<Column gap="6" padding_x="10" width="200" grow="true" surface="alt"/>)", &report);
  ST_REQUIRE(tree != nullptr);
  if (!report.clean()) st::print("[xml-ui] 报告：{}\n", describe(report));
  ST_CHECK(report.clean());
  ST_CHECK_NEAR(tree->style().gap, 6.0f, 0.001f);
  ST_CHECK_NEAR(tree->style().padding.left, 10.0f, 0.001f);
  ST_CHECK_NEAR(tree->style().width, 200.0f, 0.001f);
  ST_CHECK(tree->style().grow);
  ST_CHECK(tree->surface() == st::ui::Element::Surface::Alt);
}

ST_TEST(xml_ui_short_names_take_their_own_value) {
  // 缺陷回归（2026-10-25，热重载轮）：`align` / `bg` / `fg` / `valign` 这几个**短名**
  // 经 `normalize_property` 改名（`align` → `align_items`），而取值曾按**改名后**的名字
  // 去读 `attr_or`——XML 里当然没有那个键，于是拿到空串、`parse_align("")` 必失败。
  // 现象：**每一个短名属性都被报成“值没被接受”**，而界面就是“对齐/背景色写了不生效”。
  //
  // 夹具要推到**非默认值**上：默认 `align_items` 是 `Start`，拿 `start` 去断言恒绿。
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(
      R"(<Row align="center" bg="#112233" fg="#ffffff"><Text fg="#ff0000">x</Text></Row>)",
      &report);
  ST_REQUIRE(tree != nullptr);
  if (!report.clean()) st::print("[xml-ui] 报告：{}\n", describe(report));
  ST_CHECK(report.clean());
  ST_CHECK(tree->style().align_items == st::ui::Align::Center);
  // 字面色取值：`bg` → `background`，落成具体的 `math::Color`（不是默认的全透明）。
  const st::math::Color background = tree->style().background;
  ST_CHECK_EQ(background.r, std::uint8_t{0x11});
  ST_CHECK_EQ(background.g, std::uint8_t{0x22});
  ST_CHECK_EQ(background.b, std::uint8_t{0x33});
}

ST_TEST(xml_ui_row_and_column_map_to_panel_direction) {
  st::ui::XmlBuildReport report;
  auto row = st::ui::build_from_xml("<Row/>", &report);
  auto column = st::ui::build_from_xml("<Column/>", &report);
  ST_REQUIRE(row != nullptr);
  ST_REQUIRE(column != nullptr);
  ST_CHECK(row->style().direction == st::ui::FlexDirection::Row);
  ST_CHECK(column->style().direction == st::ui::FlexDirection::Column);
  // 别名表：`Icon` 就是注册名（`IconView` 的注册名是 `Icon`，不是类名）。
  // ⚠ 属性名是 **`icon`**（属性面的名字），不是 `name`——写 `name` 会被如实报出来。
  auto icon = st::ui::build_from_xml(R"(<Icon icon="search"/>)", &report);
  ST_REQUIRE(icon != nullptr);
  ST_CHECK(icon->type() == std::string_view("Icon"));
  ST_CHECK(report.clean());
}

ST_TEST(xml_ui_unknown_tag_is_reported_not_silent) {
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(R"(<Column><Buton>确定</Buton><Text>x</Text></Column>)",
                                     &report);
  ST_REQUIRE(tree != nullptr);
  // 拼错的标签被跳过（容错），但**必须出现在报告里**。
  ST_CHECK(!report.clean());
  ST_REQUIRE(report.unknown_tags.size() == 1);
  ST_CHECK_EQ(report.unknown_tags[0], std::string("Buton"));
  ST_CHECK_EQ(tree->child_count(), std::size_t{1});   // 只有 Text 建出来了
}

ST_TEST(xml_ui_unknown_attribute_is_reported_with_line) {
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml("<Column>\n  <Text colorr=\"red\">x</Text>\n</Column>",
                                     &report);
  ST_REQUIRE(tree != nullptr);
  ST_REQUIRE(!report.unknown_attributes.empty());
  // 报告要能定位（含标签名与行号）。
  const std::string entry = report.unknown_attributes[0];
  ST_CHECK(entry.find("Text") != std::string::npos);
  ST_CHECK(entry.find("colorr") != std::string::npos);
  ST_CHECK(entry.find("2") != std::string::npos);   // 第 2 行
}

ST_TEST(xml_ui_strict_mode_fails_on_unknown) {
  st::ui::XmlBuildReport report;
  st::ui::XmlBuildOptions strict;
  strict.strict = true;
  auto tree = st::ui::build_from_xml("<Column><Nope/></Column>", &report, strict);
  // 严格模式：认不出就整体失败（CI/工具链用），而容错模式会尽力建成。
  ST_CHECK(tree == nullptr);
  ST_CHECK(!report.unknown_tags.empty());
}

ST_TEST(xml_ui_reports_parse_warning_but_still_builds) {
  st::ui::XmlBuildReport report;
  // 少了 `</Row>`：语法上的小毛病 → 容错建成，但报告里要有话说。
  auto tree = st::ui::build_from_xml("<Column><Row><Text>x</Text></Column>", &report);
  ST_REQUIRE(tree != nullptr);
  ST_CHECK(!report.parse_warning.empty());
  ST_CHECK(!report.clean());
}

ST_TEST(xml_ui_properties_go_through_apply_properties_not_a_second_path) {
  // 这条是"不分裂"的护栏：XML 写的属性必须能被 `get_property` 读回来，
  // 也就是与协议 `set` / 脚本 `set` 落在**同一个属性面**上。
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(
      R"(<Column><Button id="b" label="启动" icon="play"/></Column>)", &report);
  ST_REQUIRE(tree != nullptr);
  Element* button = find_by_id(*tree, "b");
  ST_REQUIRE(button != nullptr);
  ST_CHECK_EQ(stat_of(*button, "label"), std::string("启动"));
  ST_CHECK_EQ(stat_of(*button, "icon"), std::string("play"));
  // 反向：用 `apply_properties`（协议 set 的同一份实现）改它，读得到。
  UiRoot root;
  const st::Json changed =
      st::ui::apply_properties(root, *button, st::Json{{ "label", "已启动" }});
  ST_CHECK(!changed.empty());
  ST_CHECK_EQ(stat_of(*button, "label"), std::string("已启动"));
}

ST_TEST(xml_ui_empty_or_garbage_input_returns_null_with_reason) {
  st::ui::XmlBuildReport report;
  ST_CHECK(st::ui::build_from_xml("", &report) == nullptr);
  ST_CHECK(!report.parse_warning.empty());

  st::ui::XmlBuildReport second;
  ST_CHECK(st::ui::build_from_xml("<UnknownTop/>", &second) == nullptr);
  ST_REQUIRE(second.unknown_tags.size() == 1);
  ST_CHECK_EQ(second.unknown_tags[0], std::string("UnknownTop"));
}

ST_TEST(xml_ui_ids_are_addressable_by_selector) {
  // 建出来的树要能被选择器寻址（否则控制通道/脚本/自动化都够不着）。
  //
  // ⚠ `UiRoot::find("#id")` **带 `#`**；不带的话它当 id 直接比（`find("title")` 也能中）。
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(
      R"(<Column id="page"><Text id="title">标题</Text><Input id="name"/></Column>)",
      &report);
  ST_REQUIRE(tree != nullptr);
  UiRoot root;
  root.set_content(std::move(tree));
  root.layout(true);
  ST_CHECK(root.find("title") != nullptr);
  ST_CHECK(root.find("name") != nullptr);
  // `find` 收的是 **id**；按类型/属性查走选择器（`query`）。
  const auto selector = st::ui::Selector::parse("Input");
  ST_REQUIRE(selector.has_value());
  ST_CHECK(root.query(*selector).size() == 1);
  ST_CHECK(root.find("missing") == nullptr);
}

ST_TEST(xml_ui_multiple_top_level_nodes_warn_but_use_first) {
  st::ui::XmlBuildReport report;
  // ⚠ 用**同名同级**会歧义，所以这里两个都是 Text——取第一个符合语义。
  auto tree = st::ui::build_from_xml("<Text>一</Text><Text>二</Text>", &report);
  if (tree == nullptr) st::print("[xml-ui] 多顶层失败：{}\n", describe(report));
  ST_REQUIRE(tree != nullptr);
  ST_CHECK(!report.parse_warning.empty());   // 提示"只用了第一个"
  ST_CHECK_EQ(tree->id(), std::string(""));  // 没给 id
  // 两个顶层元素都建出来了（匿名容器 + 两个 Text）= 3 个元素。
  ST_CHECK_EQ(report.elements, std::size_t{3});
  ST_CHECK_EQ(tree->child_count(), std::size_t{2});
}
