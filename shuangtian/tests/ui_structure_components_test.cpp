/// @file ui_structure_components_test.cpp
/// @brief 包四「结构与容器」回归：`DividerLabel` / `Accordion` / `Toolbar` / `StatusBar`。
///
/// 这一包的共同点是**几何即语义**：
///
/// - 分隔线要居中于线（不是居中于行盒）、线的两端留白必须相等；
/// - 折叠只开一个 vs 各自独立、展开后命中区要跟视觉一致；
/// - 工具条溢出时「报告的折叠数」必须等于「实际没画的项数」（两个数字脱节是这类
///   组件的经典缺陷——各自看起来都对）；
/// - 状态栏左右分流，中间空白是**被压缩的那一段**。

#include "st/test/test.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/components/all.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

using st::test::FixedAdvanceTextPort;

namespace {

using st::ui::Accordion;
using st::ui::Button;
using st::ui::DividerLabel;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::StatusBar;
using st::ui::Toolbar;

struct Fixture {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port{};
  st::ui::RenderContext context{theme, &port, 0.0};

  void layout(st::ui::Element& element, float width, float height = 400.0f) {
    element.apply_theme(theme);
    st::ui::Constraints constraints{};
    constraints.max_width = width;
    constraints.max_height = height;
    constraints.available_width = width;
    constraints.available_height = height;
    element.measure(context, constraints);
    element.arrange(context, st::math::Rect{0.0f, 0.0f, width, height});
  }

  void click(st::ui::Element& element, st::math::Point point) {
    Event event{};
    event.kind = EventKind::Click;
    event.position = point;
    element.on_event(context, event);
  }
};

/// 造一个固定宽的按钮（工具条溢出测试要可控的宽）。
auto make_button(float width) -> std::unique_ptr<Button> {
  auto button = std::make_unique<Button>("x", Button::Variant::Neutral, Button::Size::Small);
  button->style().width = width;
  return button;
}

auto overlaps(st::math::Rect a, st::math::Rect b) -> bool {
  return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// DividerLabel
// ————————————————————————————————————————————————————————————————————————————

/// 线要**垂直居中于行盒**（分隔线与整行视觉中线对齐），不是贴在顶部。
ST_TEST(divider_label_line_is_vertically_centered) {
  Fixture fixture;
  DividerLabel label("分组");
  // `Align::Center`：两侧都有线（`Left` 对齐时左线段长 0，不画）。
  label.set_align(DividerLabel::Align::Center);
  fixture.layout(label, 320.0f, 22.0f);

  const auto left = label.left_line_rect();
  const auto right = label.right_line_rect();
  ST_CHECK(!left.is_empty());
  ST_CHECK(!right.is_empty());
  // 线的中心应接近 11（行盒 22 高的一半）。
  const float left_center = left.y + left.height * 0.5f;
  const float right_center = right.y + right.height * 0.5f;
  ST_CHECK(std::abs(left_center - 11.0f) < 0.6f);
  ST_CHECK(std::abs(right_center - 11.0f) < 0.6f);
}

/// 文字左右两侧的线**留白相等**（否则文字看起来偏心）。
ST_TEST(divider_label_gaps_are_symmetric) {
  Fixture fixture;
  DividerLabel label("中间");
  label.set_align(DividerLabel::Align::Center);
  fixture.layout(label, 400.0f, 22.0f);

  const auto text = label.text_rect();
  const auto left = label.left_line_rect();
  const auto right = label.right_line_rect();
  ST_CHECK(!text.is_empty());
  const float gap_left = text.x - left.right();
  const float gap_right = right.x - text.right();
  ST_CHECK(std::abs(gap_left - gap_right) < 0.6f);
  ST_CHECK(std::abs(gap_left - DividerLabel::kGap) < 0.6f);
}

/// `Left` 对齐时左线段长度为 0（不画），右线段占据剩余空间。
ST_TEST(divider_label_left_align_leaves_no_left_line) {
  Fixture fixture;
  DividerLabel label("标题");
  label.set_align(DividerLabel::Align::Left);
  fixture.layout(label, 400.0f, 22.0f);
  ST_CHECK(label.left_line_rect().is_empty());
  ST_CHECK(!label.right_line_rect().is_empty());
}

/// 右侧动作贴右缘，且不与线重叠。
ST_TEST(divider_label_action_sits_at_right_without_overlap) {
  Fixture fixture;
  DividerLabel label("历史记录");
  label.set_action("查看全部");
  fixture.layout(label, 400.0f, 22.0f);

  const auto action = label.action_rect();
  const auto right_line = label.right_line_rect();
  ST_CHECK(!action.is_empty());
  ST_CHECK(action.right() <= 400.0f + 0.01f);
  ST_CHECK(!overlaps(action, right_line));
  ST_CHECK(right_line.right() <= action.x + 0.01f);
}

/// 长标题必须**夹到可用宽**（不能溢出把线挤没）。
ST_TEST(divider_label_long_text_is_clamped) {
  Fixture fixture;
  DividerLabel label("这是一个非常非常长的分组标题它应该被夹住而不是溢出容器");
  fixture.layout(label, 200.0f, 22.0f);
  const auto text = label.text_rect();
  ST_CHECK(!text.is_empty());
  ST_CHECK(text.right() <= 200.0f + 0.01f);
}

// ————————————————————————————————————————————————————————————————————————————
// Accordion
// ————————————————————————————————————————————————————————————————————————————

/// `Single` 模式：展开第二项必须收起第一项。
ST_TEST(accordion_single_mode_keeps_one_open) {
  Accordion accordion;
  accordion.set_mode(Accordion::Mode::Single);
  accordion.add_panel("一");
  accordion.add_panel("二");
  accordion.add_panel("三");

  accordion.open(0);
  ST_CHECK(accordion.is_open(0));
  accordion.open(1);
  ST_CHECK(!accordion.is_open(0));
  ST_CHECK(accordion.is_open(1));
  ST_CHECK(accordion.open_indices().size() == 1U);
}

/// `Multiple` 模式：多项可同时展开。
ST_TEST(accordion_multiple_mode_allows_several_open) {
  Accordion accordion;
  accordion.set_mode(Accordion::Mode::Multiple);
  accordion.add_panel("一");
  accordion.add_panel("二");
  accordion.open(0);
  accordion.open(1);
  ST_CHECK(accordion.is_open(0));
  ST_CHECK(accordion.is_open(1));
  ST_CHECK(accordion.open_indices().size() == 2U);
}

/// `Single` + `allow_collapse=false`：点已展开项**不收**（始终保持一个展开）。
ST_TEST(accordion_single_without_collapse_keeps_open) {
  Accordion accordion;
  accordion.set_mode(Accordion::Mode::Single);
  accordion.set_allow_collapse(false);
  accordion.add_panel("一");
  accordion.open(0);
  ST_CHECK(accordion.is_open(0));

  accordion.toggle(0);            // 点已展开的那一项
  ST_CHECK(accordion.is_open(0)); // 不允许收起 → 仍展开

  // 允许收起时同样操作会收起。
  accordion.set_allow_collapse(true);
  accordion.toggle(0);
  ST_CHECK(!accordion.is_open(0));
}

/// 切到 `Single` 模式时，**只保留一个**已展开项（不能留下多个）。
ST_TEST(accordion_switching_to_single_keeps_only_one) {
  Accordion accordion;
  accordion.set_mode(Accordion::Mode::Multiple);
  accordion.add_panel("一");
  accordion.add_panel("二");
  accordion.open(0);
  accordion.open(1);
  ST_CHECK(accordion.open_indices().size() == 2U);

  accordion.set_mode(Accordion::Mode::Single);
  ST_CHECK(accordion.open_indices().size() == 1U);
  ST_CHECK(accordion.is_open(0));
}

/// 整行可点（不是只有箭头可点），且点标题任意 x 都生效。
ST_TEST(accordion_whole_header_is_clickable) {
  Fixture fixture;
  Accordion accordion;
  accordion.add_panel("可点的标题");
  fixture.layout(accordion, 360.0f, 200.0f);

  const auto header = accordion.header_rect(0);
  ST_CHECK(!header.is_empty());
  // 点**最右端**（离箭头最远）也应展开。
  fixture.click(accordion, st::math::Point{header.right() - 3.0f, header.center().y});
  ST_CHECK(accordion.is_open(0));
}

/// 收起的面板内容区必须是**空矩形**（不占位、不命中）。
ST_TEST(accordion_collapsed_content_has_no_rect) {
  Fixture fixture;
  Accordion accordion;
  accordion.add_panel("标题", std::make_unique<Button>("内容"));
  fixture.layout(accordion, 360.0f, 300.0f);
  ST_CHECK(accordion.content_rect(0).is_empty());   // 默认收起

  accordion.open(0);
  fixture.layout(accordion, 360.0f, 300.0f);
  ST_CHECK(!accordion.content_rect(0).is_empty());
}

/// 展开后面板总高**增加**（内容参与布局，不是浮在上面）。
ST_TEST(accordion_opening_increases_height) {
  Fixture fixture;
  Accordion accordion;
  accordion.add_panel("标题", std::make_unique<Button>("一"));
  fixture.layout(accordion, 360.0f, 300.0f);
  const float collapsed_height = accordion.measured_size().height;

  accordion.open(0);
  // 动画从 0 开始推进：先把进度推到 1（模拟若干帧）。
  for (int frame = 0; frame < 200; ++frame) {
    st::raster::Canvas canvas(360, 300);
    accordion.paint(fixture.context, canvas);
    fixture.layout(accordion, 360.0f, 300.0f);
  }
  ST_CHECK(accordion.measured_size().height > collapsed_height);
}

/// `open` 属性面相向：写入 `|` 分隔的索引集合，读回一致。
ST_TEST(accordion_property_roundtrip) {
  Accordion accordion;
  accordion.set_mode(Accordion::Mode::Multiple);
  accordion.add_panel("一");
  accordion.add_panel("二");
  accordion.add_panel("三");

  ST_CHECK(accordion.set_property("open", "0|2"));
  ST_CHECK(accordion.is_open(0));
  ST_CHECK(!accordion.is_open(1));
  ST_CHECK(accordion.is_open(2));
  ST_CHECK(accordion.get_property("open").value_or("") == "0|2");

  ST_CHECK(accordion.set_property("open", ""));
  ST_CHECK(accordion.open_indices().empty());
}

// ————————————————————————————————————————————————————————————————————————————
// Toolbar
// ————————————————————————————————————————————————————————————————————————————

/// 放得下时**不折叠**，每项都有矩形。
ST_TEST(toolbar_wide_has_no_collapse) {
  Fixture fixture;
  Toolbar bar;
  for (int i = 0; i < 3; ++i) bar.add_item(make_button(40.0f));
  fixture.layout(bar, 400.0f, 34.0f);

  ST_CHECK(bar.collapsed_count() == 0U);
  ST_CHECK(bar.overflow_rect().is_empty());
  for (std::size_t i = 0; i < 3U; ++i) ST_CHECK(!bar.item_rect(i).is_empty());
}

/// 变窄时折叠，且**报告的折叠数 == 实际没画的项数**。
///
/// ⚠ 只断言「两者相等」是不够的：即便折叠集合算错，后面的「放不下就跳过」
/// 兜底也会把超宽项跳掉，于是两个数字仍然相等（逆向验证担到的假绿）。
/// 所以这里额外断言**被折的是最右边那几项**——这是 `resolve_collapse` 的
/// 契约（从右往左折），兜底做不到这件事。
ST_TEST(toolbar_narrow_reports_same_count_as_hidden) {
  Fixture fixture;
  Toolbar bar;
  for (int i = 0; i < 6; ++i) bar.add_item(make_button(50.0f));
  fixture.layout(bar, 180.0f, 34.0f);

  ST_CHECK(bar.collapsed_count() > 0U);
  ST_CHECK(!bar.overflow_rect().is_empty());

  std::size_t hidden = 0;
  for (std::size_t i = 0; i < 6U; ++i) {
    if (bar.item_rect(i).is_empty()) ++hidden;
  }
  ST_CHECK(hidden == bar.collapsed_count());

  // 被折的必须是**后缀**：一旦某一项被折，它右边（除 pinned）的项也必须被折。
  bool seen_hidden = false;
  for (std::size_t i = 0; i < 6U; ++i) {
    const bool is_hidden = bar.item_rect(i).is_empty();
    if (is_hidden) seen_hidden = true;
    else ST_CHECK(!seen_hidden);   // 折了之后不能再有可见项（除 pinned）
  }
}

/// `pinned` 项**永不折叠**（窄到极限也保留）。
ST_TEST(toolbar_pinned_item_never_collapses) {
  Fixture fixture;
  Toolbar bar;
  bar.add_item(make_button(50.0f), /*pinned=*/true);   // 第 0 项固定
  for (int i = 0; i < 5; ++i) bar.add_item(make_button(50.0f));
  fixture.layout(bar, 140.0f, 34.0f);

  ST_CHECK(!bar.item_rect(0).is_empty());   // 固定的那项还在
  ST_CHECK(bar.collapsed_count() > 0U);
}

/// 可见各项**不重叠**，且都不越过溢出按钮。
ST_TEST(toolbar_visible_items_do_not_overlap_overflow) {
  Fixture fixture;
  Toolbar bar;
  for (int i = 0; i < 6; ++i) bar.add_item(make_button(50.0f));
  fixture.layout(bar, 200.0f, 34.0f);

  const auto overflow = bar.overflow_rect();
  ST_CHECK(!overflow.is_empty());
  std::vector<st::math::Rect> visible;
  for (std::size_t i = 0; i < 6U; ++i) {
    const auto rect = bar.item_rect(i);
    if (rect.is_empty()) continue;
    visible.push_back(rect);
    ST_CHECK(rect.right() <= overflow.x + 0.5f);   // 不压到溢出按钮
    ST_CHECK(rect.x >= 0.0f);
  }
  for (std::size_t i = 0; i < visible.size(); ++i) {
    for (std::size_t j = i + 1; j < visible.size(); ++j) {
      ST_CHECK(!overlaps(visible[i], visible[j]));
    }
  }
}

/// 点溢出按钮派发 `on_overflow`；没折叠时**不派发**。
ST_TEST(toolbar_overflow_click_only_when_collapsed) {
  Fixture fixture;
  Toolbar bar;
  for (int i = 0; i < 6; ++i) bar.add_item(make_button(50.0f));
  int calls = 0;
  bar.on_overflow = [&calls] { ++calls; };

  fixture.layout(bar, 400.0f, 34.0f);   // 宽：不折叠
  ST_CHECK(bar.overflow_rect().is_empty());
  bar.show_overflow();
  ST_CHECK(calls == 0);                 // 没折叠 → 不该弹

  fixture.layout(bar, 160.0f, 34.0f);   // 窄：折叠
  fixture.click(bar, bar.overflow_rect().center());
  ST_CHECK(calls == 1);
}

/// 分组分隔：分隔项本身有矩形（细线），且高度**比按钮矮**。
ST_TEST(toolbar_separator_is_shorter_than_items) {
  Fixture fixture;
  Toolbar bar;
  bar.add_item(make_button(40.0f));
  bar.add_separator();
  bar.add_item(make_button(40.0f));
  fixture.layout(bar, 400.0f, 34.0f);

  const auto separator = bar.item_rect(1);
  ST_CHECK(!separator.is_empty());
  const auto first = bar.item_rect(0);
  ST_CHECK(separator.height < first.height);
}

// ————————————————————————————————————————————————————————————————————————————
// StatusBar
// ————————————————————————————————————————————————————————————————————————————

/// 左区贴左、右区贴右，中间留空白。
ST_TEST(status_bar_left_and_right_are_anchored_to_edges) {
  Fixture fixture;
  StatusBar bar;
  bar.add_left({"就绪"});
  bar.add_right({"UTF-8"});
  fixture.layout(bar, 400.0f, 24.0f);

  const auto left = bar.left_rect(0);
  const auto right = bar.right_rect(0);
  ST_CHECK(!left.is_empty());
  ST_CHECK(!right.is_empty());
  // 左区从左边距开始。
  ST_CHECK(std::abs(left.x - StatusBar::kPaddingX) < 0.6f);
  // 右区贴右缘（留一个内边距）。
  ST_CHECK(std::abs(right.right() - (400.0f - StatusBar::kPaddingX)) < 0.6f);
  // 中间确实有空白。
  ST_CHECK(bar.middle_gap() > 0.0f);
  ST_CHECK(left.right() <= right.x + 0.01f);
}

/// 多个左段**从左往右递增**且互不重叠。
ST_TEST(status_bar_left_segments_are_ordered) {
  Fixture fixture;
  StatusBar bar;
  bar.add_left({"一"});
  bar.add_left({"二"});
  bar.add_left({"三"});
  fixture.layout(bar, 400.0f, 24.0f);

  float previous_right = -1.0f;
  for (std::size_t i = 0; i < 3U; ++i) {
    const auto rect = bar.left_rect(i);
    ST_CHECK(!rect.is_empty());
    ST_CHECK(rect.x >= previous_right - 0.01f);
    previous_right = rect.right();
  }
}

/// 多个右段：**最后一段最贴右**（从右往左排）。
ST_TEST(status_bar_right_segments_anchor_to_right) {
  Fixture fixture;
  StatusBar bar;
  bar.add_right({"甲"});
  bar.add_right({"乙"});
  fixture.layout(bar, 400.0f, 24.0f);

  const auto first = bar.right_rect(0);
  const auto second = bar.right_rect(1);
  ST_CHECK(!first.is_empty());
  ST_CHECK(!second.is_empty());
  ST_CHECK(second.x > first.x);                        // 后加的靠右
  ST_CHECK(std::abs(second.right() - (400.0f - StatusBar::kPaddingX)) < 0.6f);
}

/// 可点段：点它派发 id；不可点段点了**不派发**。
ST_TEST(status_bar_clickable_segment_dispatches_only_itself) {
  Fixture fixture;
  StatusBar bar;
  StatusBar::Segment plain{};
  plain.text = "只读";
  bar.add_left(plain);
  StatusBar::Segment clickable{};
  clickable.text = "行 12";
  clickable.id = "position";
  clickable.clickable = true;
  bar.add_left(clickable);
  fixture.layout(bar, 400.0f, 24.0f);

  std::string reported;
  bar.on_segment_click = [&reported](std::string_view id) { reported = std::string(id); };

  fixture.click(bar, bar.left_rect(0).center());   // 只读段
  ST_CHECK(reported.empty());
  fixture.click(bar, bar.left_rect(1).center());   // 可点段
  ST_CHECK(reported == "position");
}

/// 窄窗口时中间空白被**压到 0**（不是让两侧重叠）。
ST_TEST(status_bar_middle_gap_clamps_at_zero) {
  Fixture fixture;
  StatusBar bar;
  bar.add_left({"左边一段比较长的文字"});
  bar.add_right({"右边也很长的一段文字"});
  fixture.layout(bar, 120.0f, 24.0f);

  ST_CHECK(bar.middle_gap() >= 0.0f);
  // 两侧各自被裁到可用宽内，不溢出容器。
  const auto left = bar.left_rect(0);
  const auto right = bar.right_rect(0);
  ST_CHECK(left.x >= 0.0f);
  ST_CHECK(right.right() <= 120.0f + 0.01f);
}

/// 属性面 `left`/`right` 用 `|` 分隔往返。
ST_TEST(status_bar_property_roundtrip) {
  StatusBar bar;
  ST_CHECK(bar.set_property("left", "就绪|3 个问题"));
  ST_CHECK(bar.left_count() == 2U);
  ST_CHECK(bar.get_property("left").value_or("") == "就绪|3 个问题");
  ST_CHECK(bar.set_property("right", "UTF-8"));
  ST_CHECK(bar.right_count() == 1U);
  ST_CHECK(bar.get_property("right").value_or("") == "UTF-8");
}
