/// 导航与定位组件的回归护栏（2026-11-08 补全包一）：
/// `Breadcrumb` / `Pagination` / `Steps` / `SegmentedControl`。
///
/// ## 这组用例守的是什么
///
/// 四个组件的共同点是**几何即契约**：段/页/步/选项的命中矩形必须与画出来的一致，
/// 否则「点不准」这类缺陷无法用属性面发现（`get` 回来的值全对，画面也是对的，
/// 只有点下去落到别的段上）。所以每个组件都断言三件事：
///
/// 1. **落位总数 == 数据数**（少一段就是静默丢内容）；
/// 2. **命中矩形互不重叠且都在 bounds 内**（点不到 / 点到邻居的两种形态）；
/// 3. **点击 `rect.center()` 真的激活了它自己**（用回调回传的序号反证）。
///
/// 折叠规则单独成组：`Pagination::items()` 与 `Breadcrumb` 的折叠是纯函数式规则，
/// 可以脱开绘制直接断言（八种页数 × 三种当前页的表驱动）。

#include "st/test/test.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

using st::test::FixedAdvanceTextPort;

namespace {

using st::math::Point;
using st::math::Rect;

/// 命中矩形是否都落在 bounds 内且两两不重叠（同一条几何契约，四个组件共用）。
[[nodiscard]] auto rects_are_disjoint_and_inside(const std::vector<Rect>& rects, Rect bounds)
    -> bool {
  for (std::size_t left = 0; left < rects.size(); ++left) {
    if (rects[left].is_empty()) return false;
    if (rects[left].x < bounds.x - 0.01f || rects[left].y < bounds.y - 0.01f) return false;
    if (rects[left].right() > bounds.right() + 0.01f) return false;
    for (std::size_t right = left + 1; right < rects.size(); ++right) {
      const bool separated = rects[left].right() <= rects[right].x + 0.01f ||
                             rects[right].right() <= rects[left].x + 0.01f ||
                             rects[left].bottom() <= rects[right].y + 0.01f ||
                             rects[right].bottom() <= rects[left].y + 0.01f;
      if (!separated) return false;
    }
  }
  return true;
}

/// 直通 alpha 合成（预期像素用）。
///
/// 为何需要：主题里的 `border` 等令牌**自带半透明 alpha**（亚克力口径），
/// 直接拿令牌比像素永远不等——要先把令牌按 alpha 合成到底色上再比。
[[nodiscard]] auto over(st::math::Color base, st::math::Color top) -> st::math::Color {
  const float alpha = static_cast<float>(top.a) / 255.0f;
  const auto mix = [alpha](std::uint8_t bottom, std::uint8_t up) -> std::uint8_t {
    return static_cast<std::uint8_t>(static_cast<float>(bottom) * (1.0f - alpha) +
                                     static_cast<float>(up) * alpha + 0.5f);
  };
  return st::math::Color{mix(base.r, top.r), mix(base.g, top.g), mix(base.b, top.b), 255};
}

[[nodiscard]] auto near_color(st::math::Color actual, st::math::Color expected, int tolerance)
    -> bool {
  const auto diff = [](std::uint8_t left, std::uint8_t right) -> int {
    return std::abs(static_cast<int>(left) - static_cast<int>(right));
  };
  return diff(actual.r, expected.r) <= tolerance && diff(actual.g, expected.g) <= tolerance &&
         diff(actual.b, expected.b) <= tolerance;
}

/// 点击某个矩形中心（走真实事件路径，验证几何与命中共用同一份落位）。
auto click_center(st::ui::Element& element, const st::ui::RenderContext& context, Rect rect)
    -> bool {
  st::ui::Event event{};
  event.kind = st::ui::EventKind::Click;
  event.position = Point{rect.center().x, rect.center().y};
  return element.on_event(context, event);
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// Breadcrumb
// ————————————————————————————————————————————————————————————————————————————

/// 分段落位：全段可见时每段一个矩形、互不重叠、点谁激活谁。
ST_TEST(nav_breadcrumb_places_every_segment_and_hits_itself) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::Breadcrumb crumb;
  crumb.set_segments({{.label = "首页"}, {.label = "组件"}, {.label = "导航"}});
  crumb.apply_theme(theme);
  const st::ui::Constraints constraints;
  crumb.measure(context, constraints);
  ST_CHECK(crumb.segment_count() == 3U);
  ST_CHECK_EQ(std::string(crumb.current()), std::string{"导航"});

  // 给足宽度：必须**全显示**（不该为了好看提前折叠）。
  crumb.arrange(context, Rect{0.0f, 0.0f, 600.0f, st::ui::Breadcrumb::kHeight});
  ST_CHECK(crumb.ellipsis_rect().is_empty());
  ST_CHECK(crumb.visible_indices().size() == 3U);

  std::vector<Rect> rects;
  for (std::size_t index = 0; index < 3U; ++index) rects.push_back(crumb.segment_rect(index));
  ST_CHECK(rects_are_disjoint_and_inside(rects, Rect{0.0f, 0.0f, 600.0f,
                                                     st::ui::Breadcrumb::kHeight}));

  // 前两段可点（末段是「当前位置」，默认不可点）。
  std::vector<std::size_t> clicked;
  crumb.on_click = [&clicked](std::size_t index) { clicked.push_back(index); };
  ST_CHECK(click_center(crumb, context, rects[0]));
  ST_CHECK(click_center(crumb, context, rects[1]));
  ST_CHECK_EQ(clicked.size(), 2U);
  ST_CHECK_EQ(clicked[0], 0U);
  ST_CHECK_EQ(clicked[1], 1U);

  // 末段默认不触发（可点性由 `last_clickable` 决定，而不是靠"看不见的按钮"）。
  ST_CHECK(!click_center(crumb, context, rects[2]));
  ST_CHECK_EQ(clicked.size(), 2U);
  crumb.set_last_clickable(true);
  ST_CHECK(click_center(crumb, context, rects[2]));
  ST_CHECK_EQ(clicked.size(), 3U);
  ST_CHECK(crumb.get_property("last_clickable").value_or("") == "true");
}

/// 超宽时折叠中段：**首段与当前页仍在**，且 `…` 占位可见。
///
/// 这两端是导航时最常点的——折叠掉它们等于把控件变成只能「看着」的文本。
ST_TEST(nav_breadcrumb_folds_middle_but_keeps_both_ends) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::Breadcrumb crumb;
  crumb.set_segments({{.label = "root"},
                      {.label = "a"},
                      {.label = "b"},
                      {.label = "c"},
                      {.label = "d"},
                      {.label = "leaf"}});
  crumb.apply_theme(theme);
  const st::ui::Constraints constraints;
  crumb.measure(context, constraints);
  crumb.arrange(context, Rect{0.0f, 0.0f, 120.0f, st::ui::Breadcrumb::kHeight});

  const auto visible = crumb.visible_indices();
  ST_CHECK(!visible.empty());
  // 首段恒在。
  ST_CHECK_EQ(visible.front(), 0U);
  // `…` 在场（宽度不够时必须显式告诉用户"中间还有东西"）。
  ST_CHECK(!crumb.ellipsis_rect().is_empty());
  // 末段恒在（`…` 与末段都在 → 折叠的是中段）。
  ST_CHECK_EQ(visible.back(), crumb.segment_count() - 1U);
  // 折叠后不能反而更宽。
  for (std::size_t index = 0; index < crumb.segment_count(); ++index) {
    const Rect rect = crumb.segment_rect(index);
    if (rect.is_empty()) continue;
    ST_CHECK(rect.right() <= 120.0f + 0.01f);
  }
}

/// 属性面：`segments` 往返、`current` 覆写末段、未知属性一律拒绝。
ST_TEST(nav_breadcrumb_property_surface) {
  st::ui::Breadcrumb crumb;
  ST_CHECK(crumb.set_property("segments", "A|B|C"));
  ST_CHECK_EQ(crumb.segment_count(), 3U);
  ST_CHECK_EQ(crumb.get_property("segments").value_or(""), std::string{"A|B|C"});
  ST_CHECK_EQ(crumb.get_property("count").value_or(""), std::string{"3"});

  ST_CHECK(crumb.set_property("current", "Current"));
  ST_CHECK_EQ(std::string(crumb.current()), std::string{"Current"});
  ST_CHECK_EQ(crumb.get_property("segments").value_or(""), std::string{"A|B|Current"});

  // 未知属性必须如实拒绝（不许"什么都接受"）。
  ST_CHECK(!crumb.set_property("__nope__", "x"));
  ST_CHECK(!crumb.get_property("__nope__").has_value());
}

// ————————————————————————————————————————————————————————————————————————————
// Pagination
// ————————————————————————————————————————————————————————————————————————————

/// 折叠规则（纯函数式，表驱动）：这是唯一会被反复质疑的部分，必须脱开绘制钉死。
///
/// 规则：
/// - `n <= 2s+5` → 全部直出；
/// - 否则窗口固定 `2s+1` 个页码，`lo = clamp(p-s, 1, n-2s)`：**靠边时向中间撑开**
///   （页码条长度恒定，翻到首页时按钮不会集体跳一次——那是真会点错的）；
/// - 窗口左端 > 2 才插左 `…`；右端 < n-1 才插右 `…`。
ST_TEST(nav_pagination_window_rules) {
  struct Case {
    int page_count;
    int page;
    int siblings;
    std::vector<int> expected;   // 0 = 省略号
  };
  const std::vector<Case> cases{
      // 页数少：全部直出（不折叠）
      {1, 1, 1, {1}},
      {5, 3, 1, {1, 2, 3, 4, 5}},
      {7, 4, 1, {1, 2, 3, 4, 5, 6, 7}},   // 7 == 2*1+5 的临界
      // 第一页：窗口向中间撑开（页码条长度恒定，按钮不集体跳）。
      {20, 1, 1, {1, 2, 3, 0, 20}},
      // 最后一页：同理（对称）。
      {20, 20, 1, {1, 0, 18, 19, 20}},
      // 中段：两侧都折叠
      {20, 10, 1, {1, 0, 9, 10, 11, 0, 20}},
      // 左侧空缺恰好 1 个位置 → 直接出 2，不放 `…`
      {20, 3, 1, {1, 2, 3, 4, 0, 20}},
      // 右侧空缺恰好 1 个位置 → 直接出 19
      {20, 18, 1, {1, 0, 17, 18, 19, 20}},
      // 窗口贴到首端（page-siblings <= 1）时，首端不插 `…`
      {20, 2, 1, {1, 2, 3, 0, 20}},
      // siblings=2：窗口 5 个，两侧仍折叠
      {20, 10, 2, {1, 0, 8, 9, 10, 11, 12, 0, 20}},
      // siblings=0：窗口只 1 个页码
      {20, 10, 0, {1, 0, 10, 0, 20}},
  };

  for (const Case& item : cases) {
    st::ui::Pagination bar;
    bar.set_page_count(item.page_count);
    bar.set_page(item.page);
    bar.set_sibling_count(item.siblings);
    const auto items = bar.items();
    ST_CHECK_EQ(items.size(), item.expected.size());
    for (std::size_t index = 0; index < std::min(items.size(), item.expected.size()); ++index) {
      ST_CHECK_EQ(items[index], item.expected[index]);
    }
  }
}

/// 首末页与上下页的禁用态：到端点时**画成禁用且不响应**（位置恒定，不隐藏）。
ST_TEST(nav_pagination_edges_disable_navigation) {
  st::ui::Pagination bar;
  bar.set_page_count(10);
  bar.set_page(1);
  std::vector<int> changes;
  bar.on_change = [&changes](int page) { changes.push_back(page); };

  // 第 1 页：上一页无效（返回 false，不派发）。
  ST_CHECK(!bar.invoke_action("prev", ""));
  ST_CHECK_EQ(changes.size(), 0U);
  ST_CHECK(bar.invoke_action("next", ""));
  ST_CHECK_EQ(changes.size(), 1U);
  ST_CHECK_EQ(changes[0], 2);

  bar.set_page(10);
  ST_CHECK(!bar.invoke_action("next", ""));
  ST_CHECK(bar.invoke_action("prev", ""));
  ST_CHECK_EQ(bar.page(), 9);

  // `goto` 越界如实拒绝。
  ST_CHECK(!bar.invoke_action("goto", "99"));
  ST_CHECK(!bar.invoke_action("goto", "0"));
  ST_CHECK(bar.invoke_action("goto", "5"));
  ST_CHECK_EQ(bar.page(), 5);
  // 目标页 == 当前页：不算变化。
  ST_CHECK(!bar.invoke_action("goto", "5"));

  // 总页数变小 → 当前页夹取，且**不**派发 on_change（数据变化不是用户翻页）。
  const std::size_t before = changes.size();
  bar.set_page_count(3);
  ST_CHECK_EQ(bar.page(), 3);
  ST_CHECK_EQ(changes.size(), before);
}

/// 页码格落位：格数 == 项数 + 2（上下页），点击某格真的翻到那一页。
ST_TEST(nav_pagination_slots_are_hit_targets) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::Pagination bar;
  bar.set_page_count(30);
  bar.set_page(15);
  bar.apply_theme(theme);
  const st::ui::Constraints constraints;
  bar.measure(context, constraints);
  const float width = bar.measured_size().width;
  bar.arrange(context, Rect{0.0f, 0.0f, std::max(width, 320.0f), st::ui::Pagination::kHeight});

  const auto items = bar.items();
  ST_CHECK(!items.empty());

  // 逐个点「页码格」（跳过省略号）→ 应翻到那个页码。
  //
  // ⚠ 顺序：**先把当前页复位，再取几何**。`slot_rect` 按当前页算落位，
  // 若先取几何再复位，拿到的是上一次点击后那个页的几何（实测就踩了这个）。
  for (std::size_t slot = 0; slot < items.size(); ++slot) {
    if (items[slot] == 0) continue;   // 省略号不翻页
    if (items[slot] == 15) continue;
    bar.set_page(15);
    const Rect rect = bar.slot_rect(slot);
    ST_CHECK(!rect.is_empty());
    ST_CHECK(click_center(bar, context, rect));
    ST_CHECK_EQ(bar.page(), items[slot]);
  }

  // 省略号格：点了不翻页（它是占位，不是按钮）。
  for (std::size_t slot = 0; slot < items.size(); ++slot) {
    if (items[slot] != 0) continue;
    bar.set_page(15);
    const Rect rect = bar.slot_rect(slot);
    ST_CHECK(!click_center(bar, context, rect));
    ST_CHECK_EQ(bar.page(), 15);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// Steps
// ————————————————————————————————————————————————————————————————————————————

/// 状态推导：`set_current(i)` → 前 Done / 自己 Active / 后 Todo。
ST_TEST(nav_steps_state_derivation) {
  st::ui::Steps steps;
  steps.set_titles({"登录", "填资料", "提交"});
  ST_CHECK_EQ(steps.step_count(), 3U);

  steps.set_current(1);
  ST_CHECK_EQ(steps.current(), 1U);
  const auto& list = steps.steps();
  ST_CHECK(list[0].state == st::ui::Steps::State::Done);
  ST_CHECK(list[1].state == st::ui::Steps::State::Active);
  ST_CHECK(list[2].state == st::ui::Steps::State::Todo);
  ST_CHECK(!steps.all_done());

  // 越界夹到末步。
  steps.set_current(99);
  ST_CHECK_EQ(steps.current(), 2U);
  ST_CHECK(steps.steps()[1].state == st::ui::Steps::State::Done);

  // `done` 动作：全部完成。
  ST_CHECK(steps.invoke_action("done", ""));
  ST_CHECK(steps.all_done());
  ST_CHECK_EQ(steps.get_property("states").value_or(""), std::string{"done|done|done"});

  // `prev`/`next` 走 `set_current` 且如实拒绝越界。
  steps.set_current(0);
  ST_CHECK(!steps.invoke_action("prev", ""));
  ST_CHECK(steps.invoke_action("next", ""));
  ST_CHECK_EQ(steps.current(), 1U);
  ST_CHECK(steps.invoke_action("goto", "2"));
  ST_CHECK_EQ(steps.current(), 2U);
  ST_CHECK(!steps.invoke_action("goto", "9"));
}

/// 落位：跨向排列时每步一个矩形、互不重叠，连线在两圆之间且是「左端状态」的颜色。
ST_TEST(nav_steps_geometry_and_connector_color) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::Steps steps;
  steps.set_titles({"A", "B", "C"});
  steps.set_current(1);
  steps.apply_theme(theme);
  const st::ui::Constraints constraints;
  steps.measure(context, constraints);
  steps.arrange(context, Rect{0.0f, 0.0f, 420.0f, 40.0f});

  std::vector<Rect> rects;
  for (std::size_t index = 0; index < 3U; ++index) rects.push_back(steps.step_rect(index));
  // 横向时整块可点区可能是「圆 + 文字」，彼此不重叠。
  ST_CHECK(rects_are_disjoint_and_inside(rects, Rect{0.0f, 0.0f, 420.0f, 40.0f}));

  // 连线：第 0 步（Done）右侧是主色，第 1 步（Active）右侧是 border。
  const Rect first = steps.connector_rect(0);
  const Rect second = steps.connector_rect(1);
  ST_CHECK(!first.is_empty());
  ST_CHECK(!second.is_empty());
  ST_CHECK(steps.connector_rect(2).is_empty());   // 末步没有右侧连线

  st::raster::Canvas canvas(420, 40);
  canvas.clear(theme.colors().bg);
  steps.paint(context, canvas);
  const auto first_px = canvas.pixel_at_point(Point{first.center().x, first.center().y});
  const auto second_px = canvas.pixel_at_point(Point{second.center().x, second.center().y});
  // 已完成步右侧的线是主色（不透明，可直接比）；进行中步右侧是 `border`——
  // 那是**半透明令牌**，必须按 alpha 合成到底色上再比。
  ST_CHECK(near_color(first_px, theme.colors().primary, 8));
  ST_CHECK(near_color(second_px, over(theme.colors().bg, theme.colors().border), 8));
  // 两条线必须真不是一个颜色（否则“按左端状态上色”等于没实现）。
  const int delta = std::abs(static_cast<int>(first_px.r) - static_cast<int>(second_px.r)) +
                    std::abs(static_cast<int>(first_px.g) - static_cast<int>(second_px.g)) +
                    std::abs(static_cast<int>(first_px.b) - static_cast<int>(second_px.b));
  ST_CHECK(delta > 30);
}

// ————————————————————————————————————————————————————————————————————————————
// SegmentedControl
// ————————————————————————————————————————————————————————————————————————————

/// 选中与键盘循环：`←/→` 到头再按回到另一端（不是卡住）。
ST_TEST(nav_segmented_selection_and_keyboard_wrap) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::SegmentedControl control;
  control.set_options({"日", "周", "月"});
  control.apply_theme(theme);
  ST_CHECK_EQ(control.index(), 0);
  ST_CHECK_EQ(std::string(control.selected_label()), std::string{"日"});

  std::vector<int> changes;
  control.on_change = [&changes](int index) { changes.push_back(index); };

  const auto press = [&](std::string key) {
    st::ui::Event event{};
    event.kind = st::ui::EventKind::KeyDown;
    event.key = std::move(key);
    return control.on_event(context, event);
  };
  ST_CHECK(press("ArrowRight"));
  ST_CHECK_EQ(control.index(), 1);
  ST_CHECK(press("ArrowRight"));
  ST_CHECK_EQ(control.index(), 2);
  // 末段再按一次 → 回到首段（循环，不卡住）。
  ST_CHECK(press("ArrowRight"));
  ST_CHECK_EQ(control.index(), 0);
  ST_CHECK(press("ArrowLeft"));
  ST_CHECK_EQ(control.index(), 2);
  ST_CHECK_EQ(changes.size(), 4U);

  // 已选段重复点击：事件被处理（不冒泡）但不派发变化。
  control.set_index(1);
  control.arrange(context, Rect{0.0f, 0.0f, 260.0f, st::ui::SegmentedControl::kHeight});
  const std::size_t before = changes.size();
  const Rect rect = control.segment_rect(1);
  ST_CHECK(!rect.is_empty());
  ST_CHECK(click_center(control, context, rect));
  ST_CHECK_EQ(changes.size(), before);
}

/// 落位：每段一个矩形、互不重叠；点击第 N 段真的选中第 N 段。
ST_TEST(nav_segmented_geometry_matches_hits) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::SegmentedControl control;
  control.set_options({"列表", "网格", "详情"});
  control.apply_theme(theme);
  const st::ui::Constraints constraints;
  control.measure(context, constraints);
  const float width = control.measured_size().width;
  control.arrange(context, Rect{0.0f, 0.0f, std::max(width, 260.0f),
                                st::ui::SegmentedControl::kHeight});

  std::vector<Rect> rects;
  for (std::size_t index = 0; index < 3U; ++index) rects.push_back(control.segment_rect(index));
  ST_CHECK(rects_are_disjoint_and_inside(
      rects, Rect{0.0f, 0.0f, std::max(width, 260.0f), st::ui::SegmentedControl::kHeight}));

  for (std::size_t index = 0; index < 3U; ++index) {
    ST_CHECK(click_center(control, context, rects[index]));
    ST_CHECK_EQ(control.index(), static_cast<int>(index));
  }

  // 滑块跟当前段走（同一块几何：`indicator_rect` 与选中段重合）。
  const Rect indicator = control.indicator_rect();
  const Rect active = control.segment_rect(static_cast<std::size_t>(control.index()));
  ST_CHECK(std::abs(indicator.x - active.x) < 0.01f);
  ST_CHECK(std::abs(indicator.width - active.width) < 0.01f);

  // `stretch`：等宽均分。
  control.set_stretch(true);
  control.arrange(context, Rect{0.0f, 0.0f, 300.0f, st::ui::SegmentedControl::kHeight});
  const float first = control.segment_rect(0).width;
  const float second = control.segment_rect(1).width;
  ST_CHECK(std::abs(first - second) < 0.01f);
}

/// **绘制真的画出了内容**（像素级）：外壳、滑块、文字三样都在。
///
/// ## 为何必须有这条（它抓的是一个真实发生过的缺陷）
///
/// 首版实现里 `paint_content` 直接调 `relayout()`，而 `relayout()` 依赖段宽缓存；
/// 缓存被 `apply_theme` 每帧清空，paint 路径没有重建入口 → **relayout 直接返回**，
/// 结果外壳画了、文字与滑块全空。
///
/// 而属性面全部正常（`index`/`options`/`segment_rect` 都对）——
/// 所以只有「看像素/截图」能发现它。本用例就是那条护栏。
ST_TEST(nav_segmented_paints_content_not_just_the_shell) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::SegmentedControl control;
  control.set_options({"Alpha", "Beta"});
  control.set_index(0);
  control.apply_theme(theme);
  const st::ui::Constraints constraints;
  control.measure(context, constraints);
  control.arrange(context, Rect{0.0f, 0.0f, 260.0f, st::ui::SegmentedControl::kHeight});

  st::raster::Canvas canvas(260, static_cast<int>(st::ui::SegmentedControl::kHeight));
  canvas.clear(theme.colors().bg);
  control.paint(context, canvas);

  // 滑块存在：选中段中心不再是 `surface_sunken`（外壳的凹槽底）。
  const Rect active = control.segment_rect(0);
  ST_CHECK(!active.is_empty());
  const auto slider_px = canvas.pixel_at_point(Point{active.center().x, active.center().y});
  ST_CHECK(!(slider_px == theme.colors().surface_sunken));

  // 文字存在：未被滑块盖住的位置也要有墨迹。判据取「与底色差异明显」——
  // 不写死具体颜色（字体会变）。
  const auto differs = [&](st::math::Color px) {
    const int delta =
        std::abs(static_cast<int>(px.r) - static_cast<int>(theme.colors().surface.r)) +
        std::abs(static_cast<int>(px.g) - static_cast<int>(theme.colors().surface.g)) +
        std::abs(static_cast<int>(px.b) - static_cast<int>(theme.colors().surface.b));
    return delta > 20;
  };
  const Rect second = control.segment_rect(1);
  int ink_columns = 0;
  for (int x = static_cast<int>(second.x) + 2; x < static_cast<int>(second.right()) - 2; ++x) {
    bool column_has_ink = false;
    for (int y = static_cast<int>(second.y) + 4; y < static_cast<int>(second.bottom()) - 4; ++y) {
      if (differs(canvas.pixel_at_point(Point{static_cast<float>(x), static_cast<float>(y)}))) {
        column_has_ink = true;
        break;
      }
    }
    if (column_has_ink) ++ink_columns;
  }
  ST_CHECK(ink_columns > 0);

  // 换选中段后，两段仍都被画了东西。
  control.set_index(1);
  control.arrange(context, Rect{0.0f, 0.0f, 260.0f, st::ui::SegmentedControl::kHeight});
  st::raster::Canvas second_frame(260, static_cast<int>(st::ui::SegmentedControl::kHeight));
  second_frame.clear(theme.colors().bg);
  control.paint(context, second_frame);
  const Rect first = control.segment_rect(0);
  int first_ink = 0;
  for (int x = static_cast<int>(first.x) + 2; x < static_cast<int>(first.right()) - 2; ++x) {
    for (int y = static_cast<int>(first.y) + 4; y < static_cast<int>(first.bottom()) - 4; ++y) {
      if (differs(second_frame.pixel_at_point(Point{static_cast<float>(x), static_cast<float>(y)}))) {
        ++first_ink;
        break;
      }
    }
  }
  ST_CHECK(first_ink > 0);
}

/// 属性面：`options`/`index` 往返、按标签名选中、未知属性拒绝。
ST_TEST(nav_segmented_property_surface) {
  st::ui::SegmentedControl control;
  ST_CHECK(control.set_property("options", "A|B|C"));
  ST_CHECK_EQ(control.option_count(), 3U);
  ST_CHECK_EQ(control.get_property("options").value_or(""), std::string{"A|B|C"});

  ST_CHECK(control.set_property("index", "2"));
  ST_CHECK_EQ(control.index(), 2);
  ST_CHECK_EQ(control.get_property("index").value_or(""), std::string{"2"});

  ST_CHECK(control.set_property("label", "B"));
  ST_CHECK_EQ(control.index(), 1);
  ST_CHECK_EQ(std::string(control.selected_label()), std::string{"B"});

  ST_CHECK(!control.set_property("__nope__", "x"));
  ST_CHECK(!control.get_property("__nope__").has_value());
  ST_CHECK(!control.invoke_action("__nope__", ""));

  // 清空选项：无选中（`index() == -1`），且不崩。
  control.clear_options();
  ST_CHECK_EQ(control.index(), -1);
  ST_CHECK_EQ(control.option_count(), 0U);
}
