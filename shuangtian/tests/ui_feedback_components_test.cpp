/// @file ui_feedback_components_test.cpp
/// @brief 包二「反馈与状态」回归：`EmptyState` / `Banner` / `Skeleton` / `StatusDot` /
///        `AvatarGroup`。
///
/// 每条用例都对应一次**真实会犯的错**，不是“调用了不崩”。重点在三类：
///
/// 1. **几何**（落位、重叠、对齐）——属性面全对而画面错的缺陷，只能靠矩形断言抓；
/// 2. **溢出的边界**（`AvatarGroup` 的 `+N`、`Banner` 长文折行）——最容易“正常尺寸好看、
///    一挤就散”；
/// 3. **派生状态**（`tone` → 颜色/图标、`state` → 是否呼吸）——映射写错会静默错色。
///
/// 桩用 `tests/support/text_port_fixtures.hpp` 的共享 `FixedAdvanceTextPort`
///（每字符定宽，几何可手算），不自己再写一份。

#include "st/test/test.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/all.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

using st::test::FixedAdvanceTextPort;

namespace {

using st::ui::AvatarGroup;
using st::ui::Banner;
using st::ui::EmptyState;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::Skeleton;
using st::ui::StatusDot;
using st::ui::Tone;

/// 组装主题 + 桩端口 + 上下文，并提供一次到位的 “measure + arrange”。
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
  /// 画一遍（供像素/绘制断言）。
  void paint(const st::ui::Element& element, st::raster::Canvas& canvas) const {
    const_cast<st::ui::Element&>(element).paint(context, canvas);
  }
};

/// 两个矩形相交（边界接触不算）。
auto overlaps(st::math::Rect a, st::math::Rect b) -> bool {
  return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// EmptyState
// ————————————————————————————————————————————————————————————————————————————

/// 空态的图标、标题、动作三块**自上而下且互不重叠**——这是空态唯一的结构约束。
ST_TEST(empty_state_stacks_icon_above_action_without_overlap) {
  Fixture fixture;
  EmptyState view("还没有项目", "创建第一个项目开始");
  view.set_action("新建项目");
  fixture.layout(view, 360.0f, 320.0f);

  const auto icon = view.icon_rect();
  const auto action = view.action_rect();
  ST_CHECK(!icon.is_empty());            // 默认有图标
  ST_CHECK(!action.is_empty());          // 有文案就有按钮矩形
  ST_CHECK(icon.bottom() <= action.y);   // 图标在按钮之上、不重叠
  ST_CHECK(action.bottom() <= 320.0f + 0.5f);   // 不出界

  // 动作按钮水平居中：左右留白相等（组件 x = 0，宽 360）。
  ST_CHECK(std::abs(action.x - (360.0f - action.right())) < 1.0f);
}

/// 没有动作文案时**不能**留下空按钮矩形（否则命中区凭空多一块）。
ST_TEST(empty_state_without_action_leaves_no_hit_rect) {
  Fixture fixture;
  EmptyState view("空", "没有任何动作");
  fixture.layout(view, 320.0f, 240.0f);
  ST_CHECK(view.action_rect().is_empty());

  bool called = false;
  view.on_action = [&called] { called = true; };
  view.activate();
  ST_CHECK(!called);   // 没有按钮 → 主动动作不能生效
}

/// 四档尺寸的图标边长必须**严格递增**（档位没区分开就是白设了）。
ST_TEST(empty_state_size_tiers_grow_monotonically) {
  Fixture fixture;
  const auto icon_side = [&](EmptyState::Size size) {
    EmptyState view("t");
    view.set_size(size);
    fixture.layout(view, 400.0f, 400.0f);
    return view.icon_rect().width;
  };
  const float small = icon_side(EmptyState::Size::Small);
  const float medium = icon_side(EmptyState::Size::Medium);
  const float large = icon_side(EmptyState::Size::Large);
  const float hero = icon_side(EmptyState::Size::Hero);
  ST_CHECK(small < medium);
  ST_CHECK(medium < large);
  ST_CHECK(large < hero);
  ST_CHECK(small >= 24.0f);   // 最小档也不能小到看不见
}

/// 动作按钮只在按钮范围内触发；点别处不触发。命中区与绘制矩形必须是同一份。
ST_TEST(empty_state_action_only_fires_inside_button) {
  Fixture fixture;
  EmptyState view("t", "d");
  view.set_action("去创建");
  fixture.layout(view, 360.0f, 300.0f);
  int hits = 0;
  view.on_action = [&hits] { ++hits; };

  Event outside{};
  outside.kind = EventKind::Click;
  outside.position = st::math::Point{2.0f, 2.0f};
  view.on_event(fixture.context, outside);
  ST_CHECK(hits == 0);   // 组件左上角不在按钮上

  Event inside{};
  inside.kind = EventKind::Click;
  inside.position = view.action_rect().center();
  view.on_event(fixture.context, inside);
  ST_CHECK(hits == 1);
  ST_CHECK(inside.handled);
}

// ————————————————————————————————————————————————————————————————————————————
// Banner
// ————————————————————————————————————————————————————————————————————————————

/// `tone` → 默认图标必须**分档**（四档一个图标 = 用户看不出通知性质）。
ST_TEST(banner_default_icon_differs_per_tone) {
  Fixture fixture;
  const auto icon_of = [&](Tone tone) {
    Banner banner("t", tone);
    fixture.layout(banner, 360.0f, 40.0f);
    return banner.get_property("icon").value_or("");
  };
  const std::string info = icon_of(Tone::Primary);
  const std::string success = icon_of(Tone::Success);
  const std::string warning = icon_of(Tone::Warning);
  const std::string danger = icon_of(Tone::Danger);
  ST_CHECK(!info.empty());
  ST_CHECK(info != success);
  ST_CHECK(success != warning);
  ST_CHECK(warning != danger);
}

/// 关闭 × 与动作按钮**不重叠**，且关闭在最右、动作在其左。
ST_TEST(banner_close_and_action_do_not_overlap) {
  Fixture fixture;
  Banner banner("版本可更新", Tone::Primary);
  banner.set_description("重启后生效");
  banner.set_action("立即重启");
  banner.set_closable(true);
  fixture.layout(banner, 480.0f, 56.0f);

  const auto close = banner.close_rect();
  const auto action = banner.action_rect();
  ST_CHECK(!close.is_empty());
  ST_CHECK(!action.is_empty());
  ST_CHECK(!overlaps(close, action));
  ST_CHECK(action.right() <= close.x + 0.5f);   // 动作在关闭左边
  ST_CHECK(close.right() <= 480.0f + 0.5f);     // 关闭不出界
}

/// 点 × 派发 `on_dismiss` 并把自身设为不可见。
ST_TEST(banner_dismiss_hides_and_notifies) {
  Fixture fixture;
  Banner banner("注意", Tone::Warning);
  banner.set_closable(true);
  fixture.layout(banner, 400.0f, 48.0f);
  int dismissed = 0;
  banner.on_dismiss = [&dismissed] { ++dismissed; };

  Event click{};
  click.kind = EventKind::Click;
  click.position = banner.close_rect().center();
  const bool handled = banner.on_event(fixture.context, click);
  ST_CHECK(handled);
  ST_CHECK(dismissed == 1);
  ST_CHECK(!banner.visible());
}

/// 长说明文案必须**折行增高**，不能只画一行就裁掉。
ST_TEST(banner_long_description_increases_height) {
  Fixture fixture;
  Banner short_banner("t", Tone::Primary);
  short_banner.set_description("短");
  fixture.layout(short_banner, 240.0f, 200.0f);

  Banner long_banner("t", Tone::Primary);
  long_banner.set_description(
      "这是一段很长的说明文字，它应该折成好几行，从而让通知条整体变高，而不是被裁掉一半。");
  fixture.layout(long_banner, 240.0f, 200.0f);

  ST_CHECK(long_banner.measured_size().height > short_banner.measured_size().height);
}

/// `closable=false` 时不该有 × 命中区。
ST_TEST(banner_not_closable_has_no_close_rect) {
  Fixture fixture;
  Banner banner("t", Tone::Primary);
  banner.set_closable(false);
  fixture.layout(banner, 400.0f, 48.0f);
  ST_CHECK(banner.close_rect().is_empty());
}

// ————————————————————————————————————————————————————————————————————————————
// Skeleton
// ————————————————————————————————————————————————————————————————————————————

/// 文本骨架的块数 = 行数；末块按比例缩短。
ST_TEST(skeleton_text_lines_and_short_last_line) {
  Fixture fixture;
  Skeleton skeleton(4);
  skeleton.set_shape(Skeleton::Shape::Text);
  skeleton.set_last_line_ratio(0.5f);
  fixture.layout(skeleton, 300.0f, 120.0f);

  const auto first = skeleton.block_rect(0);
  const auto last = skeleton.block_rect(3);
  ST_CHECK(!first.is_empty());
  ST_CHECK(!last.is_empty());
  ST_CHECK(skeleton.block_rect(4).is_empty());   // 第 5 行不存在
  ST_CHECK(last.width < first.width);            // 末行更短
  ST_CHECK(std::abs(last.width - 300.0f * 0.5f) < 2.0f);
}

/// 圆形骨架必须**正方形**（圆角按半径算，非正方形会被拉成椭圆）。
ST_TEST(skeleton_circle_block_is_square) {
  Fixture fixture;
  Skeleton skeleton;
  skeleton.set_shape(Skeleton::Shape::Circle);
  fixture.layout(skeleton, 200.0f, 60.0f);
  const auto block = skeleton.block_rect(0);
  ST_CHECK(!block.is_empty());
  ST_CHECK(std::abs(block.width - block.height) < 0.5f);
}

/// 呼吸相位随时间变化，但**首帧最亮**（首帧最暗会让人以为没加载好）。
ST_TEST(skeleton_pulse_is_bright_at_t0_and_varies) {
  Skeleton skeleton(2);
  skeleton.set_pulse(true);
  const float at_zero = skeleton.pulse_factor(0.0);
  const float at_half = skeleton.pulse_factor(static_cast<double>(Skeleton::kPulsePeriodSeconds) * 0.5);
  const float at_full = skeleton.pulse_factor(static_cast<double>(Skeleton::kPulsePeriodSeconds));
  ST_CHECK(at_zero > at_half);                               // t=0 比半周期亮
  ST_CHECK(std::abs(at_zero - at_full) < 0.01f);              // 整周期回到原位
  ST_CHECK(at_half >= 1.0f - Skeleton::kPulseDepth - 0.01f);  // 最暗也不至于全黑
  ST_CHECK(at_half < at_zero);
}

/// 关掉呼吸后相位**恒定**（静态截图可复现）。
ST_TEST(skeleton_without_pulse_is_static) {
  Skeleton skeleton(2);
  skeleton.set_pulse(false);
  ST_CHECK(std::abs(skeleton.pulse_factor(0.0) - 1.0f) < 0.001f);
  ST_CHECK(std::abs(skeleton.pulse_factor(5.0) - 1.0f) < 0.001f);
}

// ————————————————————————————————————————————————————————————————————————————
// StatusDot
// ————————————————————————————————————————————————————————————————————————————

/// 五种状态画出来的**实际像素颜色**必须互不相同（映射撞色 = 用户分不出状态）。
///
/// 这条只能靠像素：属性面 `state` 全部正确也可能是同一个颜色。
ST_TEST(status_dot_states_paint_distinct_colors) {
  Fixture fixture;
  const std::vector<StatusDot::State> states{StatusDot::State::Ok, StatusDot::State::Warn,
                                             StatusDot::State::Error, StatusDot::State::Idle,
                                             StatusDot::State::Busy};
  std::vector<std::uint32_t> seen;
  for (const auto state : states) {
    StatusDot dot("", state);
    dot.set_pulse(false);   // 关呼吸：避免光晕把中心色拉淡，取样不稳定
    fixture.layout(dot, 40.0f, 20.0f);

    st::raster::Canvas canvas(60, 24);
    canvas.clear(fixture.theme.colors().surface);
    fixture.paint(dot, canvas);

    const auto center = dot.dot_rect().center();
    const auto pixel = canvas.pixel_at(static_cast<int>(center.x), static_cast<int>(center.y));
    const auto packed = (static_cast<std::uint32_t>(pixel.r) << 16) |
                        (static_cast<std::uint32_t>(pixel.g) << 8) |
                        static_cast<std::uint32_t>(pixel.b);
    for (const auto other : seen) ST_CHECK(other != packed);
    seen.push_back(packed);
  }
  ST_CHECK(seen.size() == states.size());
}

/// `Busy` 自动开呼吸，其余默认不呼吸；切走时**关掉**（避免常驻续帧）。
ST_TEST(status_dot_busy_pulses_and_leaving_stops_it) {
  StatusDot dot;
  dot.set_state(StatusDot::State::Busy);
  ST_CHECK(dot.pulse());
  dot.set_state(StatusDot::State::Ok);
  ST_CHECK(!dot.pulse());
  dot.set_state(StatusDot::State::Error);
  ST_CHECK(!dot.pulse());
}

/// 点必须是**正方形**（圆），且落在组件内靠左（标签在右）。
ST_TEST(status_dot_is_square_and_inside) {
  Fixture fixture;
  StatusDot dot("在线", StatusDot::State::Ok);
  fixture.layout(dot, 120.0f, 24.0f);
  const auto rect = dot.dot_rect();
  ST_CHECK(!rect.is_empty());
  ST_CHECK(std::abs(rect.width - rect.height) < 0.01f);
  ST_CHECK(rect.x >= 0.0f);
  ST_CHECK(rect.right() <= 120.0f);
  ST_CHECK(rect.y >= 0.0f);
  ST_CHECK(rect.bottom() <= 24.0f + 0.5f);
}

/// 状态属性面**往返一致**（含别名），未知值必须被拒绝（不能静默当 idle）。
ST_TEST(status_dot_property_roundtrip_with_aliases) {
  StatusDot dot;
  ST_CHECK(dot.set_property("state", "offline"));
  ST_CHECK(dot.state() == StatusDot::State::Error);
  ST_CHECK(dot.get_property("state").value_or("") == "error");

  ST_CHECK(dot.set_property("state", "pending"));
  ST_CHECK(dot.state() == StatusDot::State::Busy);

  ST_CHECK(!dot.set_property("state", "whatever"));

  ST_CHECK(dot.set_property("label", "服务不可用"));
  ST_CHECK(dot.get_property("label").value_or("") == "服务不可用");
}

// ————————————————————————————————————————————————————————————————————————————
// AvatarGroup
// ————————————————————————————————————————————————————————————————————————————

/// 全部放得下时不出现 `+N`；放不下时出现，且 `visible + overflow == count`（不丢人）。
ST_TEST(avatar_group_overflow_sums_to_total) {
  Fixture fixture;
  AvatarGroup group;
  group.set_members({"A", "B", "C", "D", "E", "F", "G", "H"});
  group.set_avatar_size(28.0f);

  fixture.layout(group, 400.0f, 32.0f);   // 宽：8 个 × 28 × 0.72 ≈ 161，放得下
  ST_CHECK(group.overflow_count() == 0U);
  ST_CHECK(group.visible_count() == 8U);
  ST_CHECK(group.overflow_rect().is_empty());

  fixture.layout(group, 90.0f, 32.0f);    // 窄：放不下
  ST_CHECK(group.overflow_count() > 0U);
  ST_CHECK(group.visible_count() + group.overflow_count() == 8U);
  ST_CHECK(!group.overflow_rect().is_empty());
}

/// 相邻头像**必须重叠**（不重叠就不是「头像组」，而是一排孤立的圆），且重叠量可预测。
ST_TEST(avatar_group_avatars_overlap_by_ratio) {
  Fixture fixture;
  AvatarGroup group;
  group.set_members({"A", "B", "C"});
  group.set_avatar_size(28.0f);
  fixture.layout(group, 400.0f, 32.0f);

  const auto first = group.avatar_rect(0);
  const auto second = group.avatar_rect(1);
  ST_CHECK(!first.is_empty());
  ST_CHECK(!second.is_empty());
  ST_CHECK(second.x > first.x);            // 递增排布
  ST_CHECK(second.x < first.right());      // 但确有重叠
  const float overlap = first.right() - second.x;
  ST_CHECK(std::abs(overlap - 28.0f * AvatarGroup::kOverlapRatio) < 0.5f);
}

/// `max` 显式给定时优先生效（宽度足够也照砍）。
ST_TEST(avatar_group_explicit_max_caps_visible) {
  Fixture fixture;
  AvatarGroup group;
  group.set_members({"A", "B", "C", "D", "E"});
  group.set_max_visible(2);
  group.set_avatar_size(28.0f);
  fixture.layout(group, 600.0f, 32.0f);

  ST_CHECK(group.visible_count() == 2U);
  ST_CHECK(group.overflow_count() == 3U);
  ST_CHECK(!group.avatar_rect(1).is_empty());
  ST_CHECK(group.avatar_rect(2).is_empty());   // 第 3 个不画
}

/// 成员清空后既无头像也无徽标（不能残留上一帧的矩形去画）。
ST_TEST(avatar_group_clear_leaves_no_blocks) {
  Fixture fixture;
  AvatarGroup group;
  group.set_members({"A", "B"});
  fixture.layout(group, 200.0f, 32.0f);
  ST_CHECK(!group.avatar_rect(0).is_empty());

  group.clear_members();
  fixture.layout(group, 200.0f, 32.0f);
  ST_CHECK(group.avatar_rect(0).is_empty());
  ST_CHECK(group.overflow_rect().is_empty());
  ST_CHECK(group.count() == 0U);
}

/// 属性面 `members` 用 `|` 分隔往返。
ST_TEST(avatar_group_members_property_roundtrip) {
  AvatarGroup group;
  ST_CHECK(group.set_property("members", "张三|李四|王五"));
  ST_CHECK(group.count() == 3U);
  ST_CHECK(group.get_property("members").value_or("") == "张三|李四|王五");
  ST_CHECK(group.members()[1] == "李四");
}
