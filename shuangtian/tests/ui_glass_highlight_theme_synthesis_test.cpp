/// 玻璃高光的**主题合成**必须跟上「样式串回放」。
///
/// ## 症状（用户报「小白的气泡样式不一致」）
///
/// 界面里有两条气泡、同一份 `kBubbleStyle`（`highlight 0.85` + `theme_glass 1`
/// + `radius 14` + `blur 8` + `refraction 6`），却一条顶边是**亮蓝**
/// （实测 `rgb(160,192,254)`）、另一条顶边是**白色**（`rgb(253,251,248)`）。
/// 用户原话：「有的有蓝光边框有的是白色的，页面变动后会恢复蓝色边框」
/// ——「页面变动后恢复」这一句既是取证关键，也是缺陷的指纹。
///
/// ## 根因
///
/// 样式串里只写**强度**（`highlight 0.85`）时，`apply_declaration` 往
/// `Style::top_highlight` 写的是**白色占位** `{255,255,255,a}`——真正的颜色由
/// `Element::apply_theme_glass` 拿主题的 `Metrics::glass_highlight` 合成
/// （亮色主题 `#8FB4FF`，即实测的 `143,180,255`）。
///
/// 而 `apply_theme_glass` 只在**主题版本变了 / 本元素申请了刷新**时才跑
/// （`ui_root.cpp` 的 `apply_theme_tree` 逐元素判定）。样式串是声明式**每帧回放**
/// 的：它把 `style_.top_highlight` 重写成白色占位之后，若该元素这一帧不需重算
/// 主题，合成就**再也不会跑**，白色占位直接上屏。
///
/// 于是缺陷只在「同 key 复用 + 本帧未申请刷新」的路径上出现——看起来像
/// 「有时有蓝边框、有时没有」，一旦页面变动（申请了刷新）就自己好了。
///
/// ## 这些用例钉什么
///
/// ① 写高光存档必须**申请主题刷新**（`theme_refresh_requested()` 为真）——
/// 它是 `apply_theme_tree` 决定要不要重跑 `refresh_theme` 的唯一信号。
/// ② 通过属性面（`set_property("style", …)`，声明式每帧回放走的正是这条）
/// 重放同一个样式串后，高光**仍是主题色**而不是白色占位。
/// ③ 显式给了颜色时，主题不得覆盖（同一条契约的另一半）。
///
/// ⚠ **逆向验证必须走 ② 那条**：只验 ①（那一句 `request_theme_refresh()`
/// 在不在）的话，把实现改回“不申请”也能绿——因为 `set_top_highlight_override`
/// 之外的路径可能碰巧也申请了刷新。实测（2026-11-28）：早先那版用例就栽在这里，
/// 删掉修复仍然 PASS。
#include <memory>
#include <optional>
#include <string>

#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/stylesheet.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using namespace st::ui;
using namespace st::ui::dsl;
namespace math = st::math;

/// 单元素挂载：直接给一个 `Card`，不经 `Component::build`。
///
/// 为什么不用 `dsl::mount` 走声明式：那条路径每帧重建组件、`apply_box_props`
/// 每帧重放样式串，会把缺陷“盖”在刷新里（每次重放都顺带申请刷新）。
/// 本用例要的是**先稳定下来，再单独回放一次样式串**，所以直接用 `UiRoot::set_content`。
auto make_card(UiRoot& root) -> Element* {
  auto card_element = std::make_unique<Card>();
  card_element->set_radius(14.0f);
  Element* raw = card_element.get();
  root.set_content(std::move(card_element));
  return raw;
}

}  // namespace

ST_TEST(glass_highlight_override_requests_theme_refresh) {
  // **最小失效单元**：写高光存档就必须申请主题刷新。
  //
  // 这是缺陷的直接机理，也是最快能定位到修复点的一条（读它就知道为什么白）。
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  Element* card_element = make_card(root);
  ST_REQUIRE(card_element != nullptr);
  root.layout(true);

  // 清掉上一轮可能残留的请求位（`layout` 已消费过）。
  ST_CHECK(!card_element->theme_refresh_requested());

  // 只给强度：颜色留白，等主题合成——正是 `highlight 0.85` 的语义。
  card_element->set_top_highlight_override(math::Color{255, 255, 255, 216});
  ST_CHECK(card_element->theme_refresh_requested());
}

ST_TEST(glass_highlight_survives_style_replay_after_theme_applied) {
  // **核心回归**：主题已经落过一次之后，再回放样式串，高光必须仍是主题色。
  //
  // 这正是线上路径：气泡元素被按 key 复用 → 主题版本已同步（不再重算）
  // → 声明式每帧把 `highlight 0.85` 又写一遍（白色占位）→ 没人合成 → 上屏是白。
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  Element* card_element = make_card(root);
  ST_REQUIRE(card_element != nullptr);

  const Theme theme = Theme::light();
  const math::Color expected = theme.metrics().glass_highlight;
  ST_CHECK(expected.r == 0x8FU && expected.g == 0xB4U && expected.b == 0xFFU);

  // ① 先让主题落下去，并**再把版本同步一遍**（模拟“该元素已是最新”）。
  card_element->style().theme_glass = true;
  root.set_theme(theme);
  root.layout(true);
  root.layout(true);   // 第二遍：版本已同步，此后不该再靠“版本变了”重算

  // ② 单独回放样式串（= 声明式每帧做的事）。
  //
  // 直接用 `apply_stylesheet` 而不走 `set_property("style", …)`：属性面那条
  // 要求参数是 JSON 值，传裸串会拿到 `false`（且回包不报原因）——用例就成了
  // “没回放就断言”，看上去绿、实际什么都没验（实测踩过一次）。
  SheetReport report;
  const std::size_t applied = apply_stylesheet(*card_element, "highlight 0.85", &report);
  ST_REQUIRE(applied == 1U);

  // ③ 走一帧：`apply_theme_tree` 只在「版本变了 / 申请了刷新」时重算。
  root.layout(true);

  const Style& style = card_element->style();
  ST_CHECK(style.top_highlight.r == expected.r);
  ST_CHECK(style.top_highlight.g == expected.g);
  ST_CHECK(style.top_highlight.b == expected.b);
  ST_CHECK(style.top_highlight.a != 0U);
}

ST_TEST(glass_highlight_explicit_color_skips_theme_synthesis) {
  // **反向护栏**：显式给了颜色就不许被主题盖掉（同一条契约的另一半）。
  //
  // 没有这条，把 `set_top_highlight_color_declared` 的判断删掉也能让上面那条绿
  // ——那时所有高光都被统一成主题蓝，「这块玻璃就要这个光」的写法静默失效。
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  Element* card_element = make_card(root);
  ST_REQUIRE(card_element != nullptr);

  card_element->style().theme_glass = true;
  root.set_theme(Theme::light());
  root.layout(true);

  SheetReport report2;
  (void)apply_stylesheet(*card_element, "highlight 0.85, highlight_color #FF7800", &report2);
  root.layout(true);
  root.layout(true);

  const Style& style = card_element->style();
  ST_CHECK(style.top_highlight.r == 0xFFU);
  ST_CHECK(style.top_highlight.g == 0x78U);
  ST_CHECK(style.top_highlight.b == 0x00U);
}
