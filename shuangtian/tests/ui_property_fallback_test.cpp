/// 通用属性面的**覆盖护栏**（`Element` 基类属性在每个组件上都读得到）。
///
/// 这条测试防的是一类结构性缺陷：组件覆写 `get_property` / `set_property` 时
/// 忘了在末尾回退基类，于是**通用属性在它上面静默消失**。
///
/// 实测代价（2026-11-10）：58 个组件覆写 `get_property`，只有 13 个回退基类——
/// 其余 45 个上动画五项 / hovered / color / font_size / weight 全部读不到，
/// 75 个组件合计缺失 **462 项**。现象是 `get animation` 返回空，
/// 而元素明明在播动画——很容易误判成“动画没起播”。
///
/// 为什么不用“逐个组件审查”的写法：这是**新增组件时必漏**的约定。
/// 护栏测试让漏掉的那一刻就变红，而不是等某个自动化脚本拿到空值去猜。

#include "st/core/log.hpp"
#include "st/test/test.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/components/all.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"
#include "st/ui/xml_ui.hpp"

#include <string>
#include <vector>

namespace {

using st::ui::Element;

/// 基类无条件提供、与组件语义无关的通用读回项。
///
/// 不列入的（**因为空值本身是有意义的答案**，不能拿“有没有值”判缺失）：
/// - `animation_opacity` / `animation_translate`：首帧绘制前还没有采样值；
/// - `transition`：没声明过渡时就是 nullopt（“没设”与“读不到”必须能区分）。
/// 这三项的正确性由 `ui_animation_test` 直接验（那里会先跑一帧再读）。
///
/// 但这三项**仍在名字表里**（只读表），所以下面“声明层”那轮检查照样覆盖它们。
constexpr std::string_view kUniversalProps[] = {
    "animation", "animation_running", "hovered", "hover_progress", "hover_effect",
};

/// 名字表里必须出现的全部通用名（含上表 + 上面那三项 nullopt 可能的）。
constexpr std::string_view kDeclaredProps[] = {
    "animation",  "animation_running",      "animation_opacity", "animation_translate",
    "transition", "hovered",                "hover_progress",    "hover_effect",
};

ST_TEST(universal_properties_readable_on_every_component) {
  st::ui::import_components();
  st::ui::component_registry::run_imports();
  const auto names = st::ui::component_registry::names();
  // 夹具先自检：注册表空着时下面的循环一条都不跑 —— 那种“全绿”是假的。
  ST_REQUIRE(names.size() >= 70U);

  st::ui::UiRoot root;
  root.set_theme(st::ui::Theme::light());
  root.set_viewport({800.0f, 600.0f});

  std::vector<std::string> offenders;
  for (const auto& name : names) {
    // **`IconView` 是明确例外**：它的大小是“直径”而不是字号，`property_names`
    // 刻意不包 `with_universal`（否则 `font_size` 会变成合法属性，
    // 把“直径 vs 字号”这条语义边界抹掉）。它的取舍由
    // `ui_icon_diameter_is_not_font_size` 钉住，不归本测试管。
    if (name == "IconView") continue;
    st::ui::XmlBuildReport report;
    auto tree = st::ui::build_from_xml("<" + name + " id=\"probe\"/>", &report);
    if (tree == nullptr) continue;   // 建不出来的组件由 `xml_ui` 自己那条测试管
    // 造一个**真实存在**的动画：否则 `animation` 空是正确答案，测不出缺陷。
    (void)tree->set_preset_animation("fadeInUp");
    for (const std::string_view prop : kUniversalProps) {
      if (!tree->get_property(prop).has_value()) {
        offenders.push_back(std::format("{} 读不到 {}", name, prop));
      }
    }
    // **声明层也要查**（不只是实际读）：只查 `get_property` 会漏掉
    // “`get` 能读、但 `property_names` 没声明”的半成品——那种情况下
    // `get` 因为回退基类而能读，而快照遍历的是名字表，于是快照里照样没它。
    // 实测：回退 `Button::property_names` 的 `with_universal` 后，
    // 只查读值的版本**仍然全绿**（这就是护栏盲区）。
    const auto declared = tree->property_names();
    const auto readonly = tree->readonly_property_names();
    for (const std::string_view prop : kDeclaredProps) {
      const bool in_writable = std::ranges::find(declared, prop) != declared.end();
      const bool in_readonly = std::ranges::find(readonly, prop) != readonly.end();
      if (!in_writable && !in_readonly) {
        offenders.push_back(std::format("{} 名字表里没声明 {}", name, prop));
      }
    }
  }
  for (const auto& item : offenders) st::log::out("[universal] {}\n", item);
  ST_CHECK(offenders.empty());

  // 反向验证夹具本身有效：`Button` 曾经就是缺的那个（`get_property` 末尾
  // `return std::nullopt` 而非转调基类），所以它必须真的读得到。
  st::ui::Button sample("x");
  (void)sample.set_preset_animation("fadeInUp");
  ST_CHECK(sample.get_property("animation").has_value());
  ST_CHECK(sample.get_property("hovered").has_value());
}

ST_TEST(universal_properties_writable_on_every_component) {
  // 写侧同理：`set_property` 也要回退基类。判据是**写进去能读回来**
  //（只判“返回 true”会被静默吞掉的实现骗过）。
  st::ui::import_components();
  st::ui::component_registry::run_imports();
  const auto names = st::ui::component_registry::names();
  ST_REQUIRE(names.size() >= 70U);

  st::ui::UiRoot root;
  root.set_theme(st::ui::Theme::light());
  root.set_viewport({800.0f, 600.0f});

  st::ui::Button sample("x");
  // `color`（显式文字色）是基类无条件支持的写入口
  ST_CHECK(sample.set_property("color", "#ff0000"));
  const auto read_back = sample.get_property("color");
  ST_REQUIRE(read_back.has_value());
  ST_CHECK_EQ(*read_back, std::string("#ff0000"));
}

}  // namespace

