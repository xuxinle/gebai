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
#include "st/core/log.hpp"
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
  for (const auto& attr : report.ineffective_attributes)
    out += std::format(" | ineffective:{}", attr);
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
      <Text id="title" text_size="18">设置</Text>
      <Row gap="6" id="actions">
        <Button id="ok" variant="primary">确定</Button>
        <Button id="cancel">取消</Button>
      </Row>
    </Column>)", &report);
  ST_REQUIRE(tree != nullptr);
  if (!report.clean()) st::log::out("[xml-ui] 报告：{}\n", describe(report));
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
  if (!report.clean()) st::log::out("[xml-ui] 报告：{}\n", describe(report));
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
  if (!report.clean()) st::log::out("[xml-ui] 报告：{}\n", describe(report));
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
  if (tree == nullptr) st::log::out("[xml-ui] 多顶层失败：{}\n", describe(report));
  ST_REQUIRE(tree != nullptr);
  ST_CHECK(!report.parse_warning.empty());   // 提示"只用了第一个"
  ST_CHECK_EQ(tree->id(), std::string(""));  // 没给 id
  // 两个顶层元素都建出来了（匿名容器 + 两个 Text）= 3 个元素。
  ST_CHECK_EQ(report.elements, std::size_t{3});
  ST_CHECK_EQ(tree->child_count(), std::size_t{2});
}

// ── 叠放定位（=`absolute`）─────────────────────────────────────────

ST_TEST(xml_ui_absolute_positioning_lands_on_style) {
  // 缺陷回归（2026-11-10）：`absolute/left/top/right/bottom` 一直在
  // `xml_ui.cpp` 的白名单里（还带着一段“上次漏了它导致图谱覆盖层消失”的注释），
  // 但 `apply_properties` 里**没有对应分支**——于是 XML 写 `absolute="true"`
  // 得到的是“的值没被接受”，把人往“值写错了”的方向引，而真相是这一层没实现。
  //
  // 夹具用**非零值**：全零时“溶上了”与“没溶”完全等价，回退修复后仍然全绿。
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(
      R"(<Column absolute="true" left="12" top="34" right="56" bottom="78"/>)", &report);
  ST_REQUIRE(tree != nullptr);
  if (!report.clean()) st::log::out("[xml-ui] 报告：{}\n", describe(report));
  ST_CHECK(report.clean());   // 名字与值都该被接受
  ST_CHECK(tree->style().absolute);
  ST_CHECK_NEAR(tree->style().left, 12.0f, 0.001f);
  ST_CHECK_NEAR(tree->style().top, 34.0f, 0.001f);
  ST_CHECK_NEAR(tree->style().right, 56.0f, 0.001f);
  ST_CHECK_NEAR(tree->style().bottom, 78.0f, 0.001f);
}

ST_TEST(xml_ui_absolute_actually_changes_layout) {
  // 上一条只验“属性进了 Style”。而 `absolute` 的意义是**改变摆放**——
  // 验不到这条就只是“写进去了但没人用”，与 `id` 那类元数据毫无区别。
  //
  // 判据：同一棵树里，一个绝对定位块应当**叠在**第一个块上（而不是排在它下面）。
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(R"(
<Column width="200" height="200">
  <Row id="base" width="200" height="100"/>
  <Row id="overlay" absolute="true" left="10" top="20" width="50" height="30"/>
</Column>)",
                                     &report);
  ST_REQUIRE(tree != nullptr);
  if (!report.clean()) st::log::out("[xml-ui] 报告：{}\n", describe(report));
  ST_CHECK(report.clean());
  UiRoot root;
  root.set_content(std::move(tree));
  root.layout(true);
  st::ui::Element* base = root.find("base");
  st::ui::Element* overlay = root.find("overlay");
  ST_REQUIRE(base != nullptr);
  ST_REQUIRE(overlay != nullptr);
  // 绝对定位：左上角 = 父内容盒左上角 + (left, top)，**不受兄弟影响**。
  ST_CHECK_NEAR(overlay->bounds().x, 10.0f, 0.5f);
  ST_CHECK_NEAR(overlay->bounds().y, 20.0f, 0.5f);
  // 若不落 absolute，它会被排在第一个块的下面（y ≈ 100）——这两个断言就区分得开。
  ST_CHECK(overlay->bounds().y < base->bounds().height - 1.0f);
}

// ── 动画（`animate` / `animate_delay` / `transition`）─────────────────

ST_TEST(xml_ui_animate_starts_animation) {
  // 起播走**动作**而不是属性面（重置起点/标脏/请求续帧），所以在 `apply_one`
  // 里单独处理。这里验两件事：名字被接受（不进 unknown_attributes）、
  // 以及元素真的进入了“在播”状态。
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(
      R"(<Column animate="fadeInUp" animate_delay="0.2"/>)", &report);
  ST_REQUIRE(tree != nullptr);
  if (!report.clean()) st::log::out("[xml-ui] 报告：{}\n", describe(report));
  ST_CHECK(report.unknown_attributes.empty());
  const auto name = tree->get_property("animation");
  ST_CHECK(name.has_value());
  if (name.has_value()) ST_CHECK_EQ(*name, std::string("fadeInUp"));
}

ST_TEST(xml_ui_animate_unknown_preset_is_reported) {
  // 拼错预设名必须报（不能静默当“没设”）——否则现象是“动画写了但不动”。
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(R"(<Column animate="fadeInUpTypo"/>)", &report);
  ST_REQUIRE(tree != nullptr);
  ST_CHECK(!report.unknown_attributes.empty());
  ST_CHECK(!tree->get_property("animation").has_value());
}

ST_TEST(xml_ui_transition_is_configured) {
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(R"(<Column transition="0.35"/>)", &report);
  ST_REQUIRE(tree != nullptr);
  if (!report.clean()) st::log::out("[xml-ui] 报告：{}\n", describe(report));
  ST_CHECK(report.clean());
  ST_CHECK(tree->transition_configured());
  const auto prop = tree->get_property("transition");
  ST_CHECK(prop.has_value());
}

// ── 语义表面档位（`surface`）：对**任何**元素都生效 ──────────────────

ST_TEST(xml_ui_surface_lands_on_components_that_own_their_background) {
  // 缺陷回归（2026-11-10）。`apply_surface` 原本只在基类 `apply_theme` 里调，
  // 而 72 个覆写 `apply_theme` 的组件里**只有 7 个转调基类**——剩下 65 个上
  // 那三层统一处理（surface / appearance 回放 / 文本覆盖）全是**死代码**。
  //
  // 现象极难查：`<Button surface="alt"/>` 报“应用成功”、`surface()` 读回也对，
  // 只有 `style().background` 仍是变体色。报告干净 = 没有任何线索。
  //
  // 修法不是让 72 个组件都记得转调（那是靠自觉保护的不变式），而是加一个
  // **非虚的统一入口** `refresh_theme`：先虚派发组件样式，再统一回放显式声明。
  //
  // ⚠ **必须走 `UiRoot::layout()` 这条真实链路**：直接调 `refresh_theme` 会绕过
  // 调度点，于是“调度点调错函数”这个缺陷测不出来（实测：把调度退回
  // `element.apply_theme(theme)` 后，直调版本的测试仍全绿）。
  const st::ui::Theme theme = st::ui::Theme::light();
  const auto alt = theme.colors().surface_alt;
  for (const std::string_view tag : {"Column", "Panel", "Button", "Text", "Card", "Input"}) {
    st::ui::XmlBuildReport report;
    const std::string xml = std::format(R"(<Column id="page"><{} id="p" surface="alt"/></Column>)",
                                       tag);
    auto tree = st::ui::build_from_xml(xml, &report);
    ST_REQUIRE(tree != nullptr);
    ST_CHECK(report.unknown_attributes.empty());
    UiRoot root;
    root.set_theme(theme);
    root.set_viewport({320.0f, 240.0f});
    root.set_content(std::move(tree));
    root.layout(true);   // ← 主题下发就在这里（`apply_theme_tree`）
    Element* target = root.find("p");
    ST_REQUIRE(target != nullptr);
    const auto bg = target->style().background;
    // `Text` 默认底色是全透明，`Card`/`Input` 各有一套；三者的“没生效”长得不一样，
    // 所以断言只能用“是否等于 surface_alt”这种**与默认值无关**的判据。
    ST_CHECK(bg.r == alt.r && bg.g == alt.g && bg.b == alt.b);
  }
}

ST_TEST(xml_ui_explicit_surface_beats_component_default) {
  // 顺序语义：组件先写自己的默认色（Button = 变体色），显式声明后落 → 盖过默认。
  // 这条定死了优先级，否则“surface 生效了”与“组件自己的色还在”两种实现都能过。
  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::Button plain("x");
  plain.apply_theme(theme);
  const auto default_bg = plain.style().background;

  st::ui::Button overridden("x");
  overridden.set_surface(Element::Surface::Sunken);
  overridden.refresh_theme(theme);
  const auto bg = overridden.style().background;
  const auto sunken = theme.colors().surface_sunken;

  ST_CHECK(bg.r == sunken.r && bg.g == sunken.g && bg.b == sunken.b);
  // 夹具必须推到“默认值不同”的位置：否则“盖过了”与“没盖”两实现等价。
  ST_CHECK(!(default_bg.r == bg.r && default_bg.g == bg.g && default_bg.b == bg.b));
}

// ── 样式系统五档（`Appearance`）────────────────────────────────

ST_TEST(xml_ui_appearance_levels_land_on_style) {
  // 五个属性都是**档位语义**（不是具体数值），必须过 `Tokens` 解析：
  // 抬升 → 表面阶 + 阴影档 + 顶部高光三联；控件尺寸 → 高 + 字 + 图标三联；
  // 密度 → padding/gap 倍率。自己拼数值就是这层样式系统要消灭的“各处为政”。
  //
  // ⚠ 每个断言都要拿**非默认值**作参照：`shadow.offset_y` 默认 0、
  // 字号默认 `font_base`、padding 默认 0——拿默认值断言等于什么都没验。
  const st::ui::Theme theme = st::ui::Theme::light();
  const auto build = [&](const std::string& xml) {
    st::ui::XmlBuildReport report;
    auto tree = st::ui::build_from_xml(xml, &report);
    return std::pair{std::move(tree), report};
  };

  // 抬升：`modal`（3）必须比 `flat`（0）阴影更强。
  {
    auto [flat, r1] = build(R"(<Column id="p" elevation="flat"/>)");
    auto [modal, r2] = build(R"(<Column id="p" elevation="modal"/>)");
    ST_REQUIRE(flat != nullptr);
    ST_REQUIRE(modal != nullptr);
    ST_CHECK(r2.unknown_attributes.empty());
    flat->refresh_theme(theme);
    modal->refresh_theme(theme);
    ST_CHECK(modal->style().shadow.offset_y > flat->style().shadow.offset_y);
  }

  // 圆角档：`pill` 必须远大于 `none`。
  {
    auto [pill, r1] = build(R"(<Column id="p" radius_mode="pill"/>)");
    auto [none, r2] = build(R"(<Column id="p" radius_mode="none"/>)");
    ST_REQUIRE(pill != nullptr);
    ST_REQUIRE(none != nullptr);
    ST_CHECK(r1.unknown_attributes.empty());
    ST_CHECK(r2.unknown_attributes.empty());
    pill->refresh_theme(theme);
    none->refresh_theme(theme);
    ST_CHECK(pill->style().radius > none->style().radius);
    ST_CHECK_NEAR(none->style().radius, 0.0f, 0.001f);
  }

  // 控件尺寸：`lg` 字号必须大于 `sm`。
  {
    auto [lg, r1] = build(R"(<Column id="p" control_size="lg"/>)");
    auto [sm, r2] = build(R"(<Column id="p" control_size="sm"/>)");
    ST_REQUIRE(lg != nullptr);
    ST_REQUIRE(sm != nullptr);
    lg->refresh_theme(theme);
    sm->refresh_theme(theme);
    ST_CHECK(lg->style().font_size > sm->style().font_size);
  }

  // 密度：`comfortable`（1.15）必须比 `compact`（0.85）间距大。
  {
    auto [comfy, r1] = build(R"(<Column id="p" density="comfortable" padding="8"/>)");
    auto [compact, r2] = build(R"(<Column id="p" density="compact" padding="8"/>)");
    ST_REQUIRE(comfy != nullptr);
    ST_REQUIRE(compact != nullptr);
    comfy->refresh_theme(theme);
    compact->refresh_theme(theme);
    ST_CHECK(comfy->style().padding.left > compact->style().padding.left);
  }
}

ST_TEST(xml_ui_appearance_fields_accumulate) {
  // “只改一个字段”必须拿旧存档补齐其余——`apply_appearance` 是**整体替换**。
  // 不补的话先写 elevation、再写 density 就把抬升抹了（现象：属性都“接受”了，
  // 但只有最后一个有放果）。
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(
      R"(<Column id="p" elevation="raised" density="compact" radius_mode="pill"/>)", &report);
  ST_REQUIRE(tree != nullptr);
  if (!report.clean()) st::log::out("[xml-ui] 报告：{}\n", describe(report));
  ST_CHECK(report.clean());
  const auto declared = tree->appearance();
  ST_REQUIRE(declared.has_value());
  ST_CHECK(declared->elevation.has_value());
  ST_CHECK(declared->density.has_value());
  ST_CHECK(declared->radius.has_value());
}

ST_TEST(xml_ui_appearance_bad_level_is_reported) {
  // 拼错档位必须报（不能静默当“没设”）——否则现象是“写了但没变”。
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(R"(<Column id="p" elevation="floating"/>)", &report);
  ST_REQUIRE(tree != nullptr);
  ST_CHECK(!report.unknown_attributes.empty());
}

ST_TEST(xml_ui_appearance_same_value_is_not_rejected) {
  // 缺陷回归：`radius_mode="none"` 在默认 `radius=0` 的元素上，
  // `apply_appearance` 返回 `changed=false`——拿它当成功判据就会把一个**合法声明**
  // 报成“值没被接受”。返回值语义是“**样式值变了没**”，而属性的是“**收下了没**”。
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(R"(<Column id="p" radius_mode="none"/>)", &report);
  ST_REQUIRE(tree != nullptr);
  if (!report.clean()) st::log::out("[xml-ui] 报告：{}\n", describe(report));
  ST_CHECK(report.clean());
}
