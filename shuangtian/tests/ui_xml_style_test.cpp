// XML 入口下 `style` 属性的端到端测试。
//
// ## 为什么要单独测 XML 这条入口
//
// 统一解析层（`apply_stylesheet`）本身有单测（`ui_stylesheet_test.cpp`），
// 但**它被接进 XML 的路径**上还有两个容易漏的环节：
//
// 1. **白名单**：`xml_ui.cpp` 的 `box_prop_names()` 决定“这个属性算不算认识”。
//    漏登记就是“XML 里写了被报未生效、且静默不落”——`glass` 当初正是这个毛病
//    （见该处注释的实测代价）。
// 2. **回读比对**：XML 建树后会回读每个属性做“收下了但没生效”的检查。
//    `style` 是复合属性、没有对应的 get 键（与 `glass` 同），
//    所以那一步应当**跳过**而不是误报。
//
// 两条都必须钉住：只测解析层的话，白名单漏登记时单测全绿而 XML 里失效。

#include <memory>
#include <string>

#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/stylesheet.hpp"
#include "st/ui/ui_root.hpp"
#include "st/ui/xml_ui.hpp"

namespace {

using namespace st::ui;

/// 用 XML 建一棵独立的元素树。
///
/// 不必挂到 `UiRoot`：`style()` 在建树时已由 `apply_properties` 写好，
/// 而本文件的判据全是样式字段（不是布局几何）——挂载只会多一步无谓的设置。
struct XmlFixture {
  XmlBuildReport report;
  std::unique_ptr<Element> tree;

  explicit XmlFixture(std::string_view xml) { tree = build_from_xml(xml, &report); }

  [[nodiscard]] auto element() -> Element* { return tree.get(); }
};

}  // namespace

ST_TEST(xml_style_attribute_is_recognized) {
  // `style` 在 XML 里**能生效且不被报错**。
  //
  // 注：这条**不能**用来钉“白名单是否登记了 `style`”——实测过，
  // 白名单漏登记时本测试**仍然绿**（因为 `apply_properties` 真的收下了值，
  // `applied=true`，而 `known` 只在 `!applied` 时才参与报警）。
  // 白名单里的 `style` 是防御性登记：它只在“应用失败”时才决定
  // 报“未知名”还是“值被拒”，让错误信息更准。
  // 真正钉“白名单与解析器不脉”的是 `xml_box_prop_names_match_stylesheet_keys`。
  XmlFixture f(
      "<Column style=\"width 300, gap 8\" padding=\"10\">"
      "<Text style=\"color #112233, size 15\">hi</text>"
      "</Column>");
  ST_REQUIRE(f.element() != nullptr);
  ST_CHECK(f.report.unknown_tags.empty());
  ST_CHECK(f.report.unknown_attributes.empty());
  // 也不能被回读比对误报（`style` 是复合属性，没有对应的 get 键）。
  ST_CHECK(f.report.ineffective_attributes.empty());
}

ST_TEST(xml_style_attribute_actually_applies) {
  // **真的落上了吗**：白名单通过 ≠ 生效。
  XmlFixture f("<Column style=\"width 300, gap 8, radius 12\" padding=\"10\"/>");
  ST_REQUIRE(f.element() != nullptr);
  const Style& s = f.element()->style();
  ST_CHECK(s.width == 300.0f);
  ST_CHECK(s.gap == 8.0f);
  ST_CHECK(s.radius == 12.0f);
}

ST_TEST(xml_style_group_glass_applies) {
  // 玻璃组也要能从 XML 配（这是小白的实际用法形态）。
  XmlFixture f("<Column style=\"blur 16, refraction 6, highlight 0.85, ring 1, edges tb\"/>");
  ST_REQUIRE(f.element() != nullptr);
  const Style& s = f.element()->style();
  ST_CHECK(s.backdrop_blur == 16.0f);
  ST_CHECK(s.backdrop_refraction == 6.0f);
  ST_CHECK(s.highlight_width == 1.0f);
  ST_CHECK(s.highlight_edges == HighlightEdges::TopBottom);
}

ST_TEST(xml_style_bad_key_is_reported) {
  // 拼错的**键**在 XML 里会被报出来（进 `unknown_attributes`）。
  //
  // 实测发现：`apply_properties` 的 `style` 分支对未知键返回 false，
  // 而 XML 建树把 false 当“值没被接受”记进报告——**这正是想要的行为**：
  // XML 是静态输入，拼错应当在建树时就被看见，而不是运行时静默忽略。
  //（DSL 那一侧只记 `ST_LOG_WARN` 而不报错：声明式每帧重建，
  //   为了一个样式键阻塞整页渲染不划算。）
  XmlFixture f("<Column style=\"ratius 20\"/>");
  ST_REQUIRE(f.element() != nullptr);
  ST_CHECK(!f.report.unknown_attributes.empty());
}

ST_TEST(stylesheet_every_registered_key_is_parseable) {
  // **键表不能只“登了名字”**：`sheet_key_names()` 里每一个键都必须真的能被解析。
  //
  // 为何需要驱动式遍历（而不是逐个手写）：这一层存在的意义就是“一份键表”——
  // 而“登了名字但忘了写落位逻辑”是它唯一的新增缺陷形态。
  // 手写只能测到想到的那几个，遍历能盖住以后新加的全部。
  //
  // 取值用每个键的 `syntax` 字段里给的例子（不可能每个都能自动造，
  // 所以这里每个键给一个**通用合法值**：数字 / `1` / 合法颜色，
  // 按组的习惯选）。只要它**不被归为“未知键”**就算这一半通过——
  // 值不合法的键会被归为 `key=value`，而**键本身在表里**是确定的。
  for (const SheetKey& key : sheet_key_names()) {
    // 按语法串挑一个合法值（这个测试验的是“键能落位”，不是“值能验错”）。
    std::string value = "1";
    if (key.syntax.find('#') != std::string_view::npos) {
      value = "#808080";
    } else if (key.name == "edges" || key.name == "highlight_edges") {
      value = "all";
    } else if (key.name == "align_items" || key.name == "align_self" || key.name == "justify" ||
               key.name == "align" || key.name == "text_align") {
      value = "center";
    } else if (key.name == "weight" || key.name == "font_weight") {
      value = "bold";
    }
    // 两个键的取值形态与其他不同：
    // · `background` 是 `fill` 的别名，必须给颜色（给 `1` 不合法）
    // · `border` 要“宽度 +（可选）颜色”，单给颜色不成立
    if (key.name == "background") value = "#808080";
    if (key.name == "border") value = "1";
    const std::string text = std::string(key.name) + " " + value;
    const SheetReport report = validate_stylesheet(text);
    // 用 LIST 比对：失败时会打出实际内容（哪个键、什么值），
    // 而不是只说“有两条挂了”——后者还得逐个回查。
    ST_CHECK_LIST(report.unknown, std::vector<std::string>{});
  }
}
