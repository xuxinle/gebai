/// @file ui_widget_components_test.cpp
/// @brief 包五「展示微件」回归：`Sparkline` / `ProgressRing` / `Rating` / `Kbd` / `Link`。
///
/// 这一包全是**小尺寸图形**，缺陷几乎都属「属性全对、画出来不对」那一类：
///
/// - 迷你图：纵轴归一化错 → 波动被画成直线；笔画不内缩 → 峰谷被削平；
/// - 环：弧从 3 点开始（数学零点）→ 与钟表直觉反；
/// - 评分：半星用「画小星」→ 形状与满星不同；
/// - 键帽：平台修饰键名不一致；
/// - 链接：下划线按下伸部画 → 穿过 `g`/`p`。

#include "st/test/test.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/components/all.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

using st::test::FixedAdvanceTextPort;

namespace {

using st::ui::Event;
using st::ui::EventKind;
using st::ui::Kbd;
using st::ui::Link;
using st::ui::ProgressRing;
using st::ui::Rating;
using st::ui::Sparkline;

struct Fixture {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port{};
  st::ui::RenderContext context{theme, &port, 0.0};

  void layout(st::ui::Element& element, float width, float height = 200.0f) {
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
  void hover(st::ui::Element& element, st::math::Point point) {
    Event event{};
    event.kind = EventKind::MouseMove;
    event.position = point;
    element.on_event(context, event);
  }
  void key(st::ui::Element& element, std::string name) {
    Event event{};
    event.kind = EventKind::KeyDown;
    event.key = std::move(name);
    element.on_event(context, event);
  }
};

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// Sparkline
// ————————————————————————————————————————————————————————————————————————————

/// 纵轴按**数据自身**归一：范围窄的波动也要撑满高度（不能被画成直线）。
ST_TEST(sparkline_normalizes_to_data_range) {
  Sparkline spark(std::vector<double>{2.1, 2.15, 2.12, 2.2});
  Fixture fixture;
  fixture.layout(spark, 120.0f, 28.0f);

  ST_CHECK(std::abs(spark.data_min() - 2.1) < 1e-9);
  ST_CHECK(std::abs(spark.data_max() - 2.2) < 1e-9);
  ST_CHECK(!spark.is_flat());

  const st::math::Rect area{0.0f, 0.0f, 120.0f, 28.0f};
  const float y_min = spark.value_to_y(2.1, area);
  const float y_max = spark.value_to_y(2.2, area);
  // 最小值在下（y 大）、最大值在上（y 小），且两者几乎撑满高度。
  ST_CHECK(y_min > y_max);
  ST_CHECK(y_min - y_max > 28.0f * 0.7f);
}

/// 全等数据落在**垂直中线**（不是贴底）。
ST_TEST(sparkline_flat_data_sits_at_center) {
  Sparkline spark(std::vector<double>{5.0, 5.0, 5.0});
  Fixture fixture;
  fixture.layout(spark, 100.0f, 30.0f);

  ST_CHECK(spark.is_flat());
  const st::math::Rect area{0.0f, 0.0f, 100.0f, 30.0f};
  const float y = spark.value_to_y(5.0, area);
  ST_CHECK(std::abs(y - 15.0f) < 1.0f);
}

/// 笔画要**内缩**：极值点不能被容器边界裁掉（峰谷被削平）。
ST_TEST(sparkline_extremes_are_inset_by_half_stroke) {
  Sparkline spark(std::vector<double>{0.0, 10.0});
  Fixture fixture;
  fixture.layout(spark, 100.0f, 30.0f);

  const st::math::Rect area{0.0f, 0.0f, 100.0f, 30.0f};
  const float y_top = spark.value_to_y(10.0, area);
  const float y_bottom = spark.value_to_y(0.0, area);
  ST_CHECK(y_top > 0.0f);          // 不贴顶
  ST_CHECK(y_bottom < 30.0f);      // 不贴底
  ST_CHECK(y_top < 3.0f);          // 但仍很靠近顶部（内缩量只有半个线宽）
  ST_CHECK(y_bottom > 27.0f);
}

/// 显式范围覆盖自适应。
ST_TEST(sparkline_explicit_range_overrides) {
  Sparkline spark(std::vector<double>{1.0, 2.0, 3.0});
  spark.set_range(0.0, 10.0);
  ST_CHECK(spark.has_range());
  ST_CHECK(std::abs(spark.data_min() - 0.0) < 1e-9);
  ST_CHECK(std::abs(spark.data_max() - 10.0) < 1e-9);

  // 倒置的区间自动纠正（不静默画空图）。
  spark.set_range(10.0, 0.0);
  ST_CHECK(spark.data_min() < spark.data_max());

  spark.clear_range();
  ST_CHECK(!spark.has_range());
  ST_CHECK(std::abs(spark.data_min() - 1.0) < 1e-9);
}

/// `values` 属性面用逗号分隔往返；非法项被跳过而不是整体失败。
ST_TEST(sparkline_values_property_roundtrip) {
  Sparkline spark;
  ST_CHECK(spark.set_property("values", "1,2,3.5"));
  ST_CHECK(spark.count() == 3U);
  ST_CHECK(std::abs(spark.values()[2] - 3.5) < 1e-9);
  ST_CHECK(spark.get_property("values").value_or("") == "1,2,3.5");

  // 含非法项：跳过非法项，保留可解析的。
  ST_CHECK(spark.set_property("values", "1,abc,3"));
  ST_CHECK(spark.count() == 2U);
}

// ————————————————————————————————————————————————————————————————————————————
// ProgressRing
// ————————————————————————————————————————————————————————————————————————————

/// 值被夹到 0..1；百分比四舍五入。
ST_TEST(progress_ring_clamps_and_percent) {
  ProgressRing ring;
  ring.set_value(1.5f);
  ST_CHECK(std::abs(ring.value() - 1.0f) < 1e-6f);
  ST_CHECK(ring.percent() == 100);

  ring.set_value(-0.5f);
  ST_CHECK(std::abs(ring.value() - 0.0f) < 1e-6f);
  ST_CHECK(ring.percent() == 0);

  ring.set_value(0.723f);
  ST_CHECK(ring.percent() == 72);
}

/// 到达 100% **只触发一次**；回落再涨会重新武装。
ST_TEST(progress_ring_complete_fires_once) {
  ProgressRing ring;
  int completions = 0;
  ring.on_complete = [&completions] { ++completions; };

  ring.set_value(1.0f);
  ST_CHECK(completions == 1);
  ring.set_value(0.9f);
  ring.set_value(1.0f);
  ST_CHECK(completions == 2);   // 重新武装后再次触发
}

/// 环必须是**正圆**（非正方会画成椭圆）。
ST_TEST(progress_ring_is_circular) {
  Fixture fixture;
  ProgressRing ring;
  ring.set_value(0.5f);
  fixture.layout(ring, 120.0f, 40.0f);
  const auto size = ring.measured_size();
  ST_CHECK(std::abs(size.width - size.height) < 0.01f);
  ST_CHECK(std::abs(size.width - 40.0f) < 0.01f);   // 取短边
}

/// 不确定态的弧**随时间转动**（不是固定一段）。
ST_TEST(progress_ring_indeterminate_sweep_rotates) {
  ProgressRing ring;
  ring.set_indeterminate(true);
  const float at_zero = ring.sweep_start(0.0);
  const float at_quarter = ring.sweep_start(static_cast<double>(ProgressRing::kIndeterminatePeriod) * 0.25);
  const float at_full = ring.sweep_start(static_cast<double>(ProgressRing::kIndeterminatePeriod));
  ST_CHECK(std::abs(at_zero) < 1e-4f);            // 从 12 点开始
  ST_CHECK(at_quarter > at_zero);                 // 转动
  ST_CHECK(std::abs(at_quarter - 1.5708f) < 0.01f);  // 四分之一周期 = 90°
  ST_CHECK(std::abs(at_full - at_zero) < 1e-3f);  // 整周期回到起点

  // 确定态不转动。
  ring.set_indeterminate(false);
  ST_CHECK(std::abs(ring.sweep_start(3.0)) < 1e-6f);
}

/// 属性面 `value` 接受 `0.5` 与 `50` 两种写法（>1 按百分比解释）。
ST_TEST(progress_ring_value_accepts_percent) {
  ProgressRing ring;
  ST_CHECK(ring.set_property("value", "50"));
  ST_CHECK(std::abs(ring.value() - 0.5f) < 1e-6f);
  ST_CHECK(ring.set_property("value", "0.25"));
  ST_CHECK(std::abs(ring.value() - 0.25f) < 1e-6f);
}

// ————————————————————————————————————————————————————————————————————————————
// Rating
// ————————————————————————————————————————————————————————————————————————————

/// 值吸附到步长：不开半星时 `3.7` → `4`。
ST_TEST(rating_snaps_to_step) {
  Rating rating(5);
  rating.set_allow_half(false);
  rating.set_value(3.7f);
  ST_CHECK(std::abs(rating.value() - 4.0f) < 1e-6f);

  rating.set_allow_half(true);
  rating.set_value(3.7f);
  ST_CHECK(std::abs(rating.value() - 3.5f) < 1e-6f);
}

/// 值夹在 `0..max`。
ST_TEST(rating_clamps_to_max) {
  Rating rating(5);
  rating.set_value(99.0f);
  ST_CHECK(std::abs(rating.value() - 5.0f) < 1e-6f);
  rating.set_value(-3.0f);
  ST_CHECK(std::abs(rating.value()) < 1e-6f);
}

/// 每颗星一个**不重叠**的命中区。
ST_TEST(rating_star_rects_are_disjoint) {
  Fixture fixture;
  Rating rating(5);
  rating.set_value(2.0f);
  fixture.layout(rating, 200.0f, 24.0f);

  float previous_right = -1.0f;
  for (std::size_t i = 0; i < 5U; ++i) {
    const auto rect = rating.star_rect(i);
    ST_CHECK(!rect.is_empty());
    ST_CHECK(rect.x >= previous_right - 0.01f);
    previous_right = rect.right();
  }
}

/// 点第 3 颗星 = 3 分；开半星后点它左半边 = 2.5 分。
ST_TEST(rating_click_sets_value_with_half) {
  Fixture fixture;
  Rating rating(5);
  rating.set_allow_clear(false);   // 关掉「再点清零」，便于观察点击值
  fixture.layout(rating, 200.0f, 24.0f);

  const auto third = rating.star_rect(2);
  fixture.click(rating, st::math::Point{third.right() - 2.0f, third.center().y});
  ST_CHECK(std::abs(rating.value() - 3.0f) < 1e-5f);

  rating.set_allow_half(true);
  fixture.layout(rating, 200.0f, 24.0f);
  const auto second = rating.star_rect(1);
  fixture.click(rating, st::math::Point{second.x + 2.0f, second.center().y});
  ST_CHECK(std::abs(rating.value() - 1.5f) < 1e-5f);
}

/// 悬停只**预览**，不改值；移开后回到实际值。
ST_TEST(rating_hover_previews_without_committing) {
  Fixture fixture;
  Rating rating(5);
  rating.set_value(2.0f);
  fixture.layout(rating, 200.0f, 24.0f);

  const auto fourth = rating.star_rect(3);
  fixture.hover(rating, st::math::Point{fourth.right() - 2.0f, fourth.center().y});
  ST_CHECK(std::abs(rating.displayed_value() - 4.0f) < 1e-5f);   // 预览 4
  ST_CHECK(std::abs(rating.value() - 2.0f) < 1e-5f);             // 值没变

  // 移出所有星：预览消失，回到实际值。
  fixture.hover(rating, st::math::Point{1000.0f, 1000.0f});
  ST_CHECK(std::abs(rating.displayed_value() - 2.0f) < 1e-5f);
}

/// 只读：不预览、点击与键盘都不改值。
ST_TEST(rating_readonly_ignores_input) {
  Fixture fixture;
  Rating rating(5);
  rating.set_value(3.0f);
  rating.set_readonly(true);
  fixture.layout(rating, 200.0f, 24.0f);

  const auto fifth = rating.star_rect(4);
  fixture.hover(rating, st::math::Point{fifth.right() - 2.0f, fifth.center().y});
  ST_CHECK(std::abs(rating.displayed_value() - 3.0f) < 1e-5f);   // 不预览
  fixture.click(rating, st::math::Point{fifth.right() - 2.0f, fifth.center().y});
  ST_CHECK(std::abs(rating.value() - 3.0f) < 1e-5f);             // 不改值
  fixture.key(rating, "ArrowRight");
  ST_CHECK(std::abs(rating.value() - 3.0f) < 1e-5f);
}

/// 键盘：方向键增减、`Home`/`End` 到两端。
ST_TEST(rating_keyboard_adjusts) {
  Fixture fixture;
  Rating rating(5);
  rating.set_value(2.0f);
  fixture.layout(rating, 200.0f, 24.0f);

  fixture.key(rating, "ArrowRight");
  ST_CHECK(std::abs(rating.value() - 3.0f) < 1e-5f);
  fixture.key(rating, "ArrowLeft");
  fixture.key(rating, "ArrowLeft");
  ST_CHECK(std::abs(rating.value() - 1.0f) < 1e-5f);
  fixture.key(rating, "End");
  ST_CHECK(std::abs(rating.value() - 5.0f) < 1e-5f);
  fixture.key(rating, "Home");
  ST_CHECK(std::abs(rating.value()) < 1e-5f);
}

/// 再点当前值可清零（`allow_clear`）；关掉后不清。
ST_TEST(rating_click_same_value_clears_when_allowed) {
  Fixture fixture;
  Rating rating(5);
  rating.set_value(3.0f);
  fixture.layout(rating, 200.0f, 24.0f);
  const auto third = rating.star_rect(2);
  const st::math::Point point{third.right() - 2.0f, third.center().y};

  fixture.click(rating, point);   // 点当前值 → 清零
  ST_CHECK(std::abs(rating.value()) < 1e-5f);

  rating.set_allow_clear(false);
  rating.set_value(3.0f);
  fixture.layout(rating, 200.0f, 24.0f);
  fixture.click(rating, point);
  ST_CHECK(std::abs(rating.value() - 3.0f) < 1e-5f);   // 不清
}

// ————————————————————————————————————————————————————————————————————————————
// Kbd
// ————————————————————————————————————————————————————————————————————————————

/// 平台修饰键名：Windows 用文字、mac 用符号。
ST_TEST(kbd_platform_modifier_labels) {
  ST_CHECK(Kbd::platform_label("ctrl", Kbd::Platform::Windows) == "Ctrl");
  ST_CHECK(Kbd::platform_label("ctrl", Kbd::Platform::Mac) == "⌃");
  ST_CHECK(Kbd::platform_label("cmd", Kbd::Platform::Mac) == "⌘");
  ST_CHECK(Kbd::platform_label("cmd", Kbd::Platform::Windows) == "Win");
  ST_CHECK(Kbd::platform_label("shift", Kbd::Platform::Mac) == "⇧");
  ST_CHECK(Kbd::platform_label("shift", Kbd::Platform::Windows) == "Shift");
  ST_CHECK(Kbd::platform_label("alt", Kbd::Platform::Mac) == "⌥");
  // 大写写法同样识别。
  ST_CHECK(Kbd::platform_label("Ctrl", Kbd::Platform::Windows) == "Ctrl");
  // 未知键名原样返回（不吞掉用户给的文字）。
  ST_CHECK(Kbd::platform_label("F5", Kbd::Platform::Windows) == "F5");
}

/// `set_shortcut` 按 `+` 拆分；空段被丢掉。
ST_TEST(kbd_shortcut_parsing) {
  Kbd kbd;
  kbd.set_shortcut("Ctrl+Shift+S");
  ST_CHECK(kbd.keys().size() == 3U);
  ST_CHECK(kbd.keys()[0] == "Ctrl");
  ST_CHECK(kbd.keys()[2] == "S");

  kbd.set_shortcut("Ctrl++S");   // 空段
  ST_CHECK(kbd.keys().size() == 2U);
}

/// 键帽**不重叠**，且宽度随内容变化（`Ctrl` 比 `S` 宽）。
ST_TEST(kbd_caps_are_disjoint_and_size_to_content) {
  Fixture fixture;
  Kbd kbd;
  kbd.set_shortcut("Ctrl+S");
  fixture.layout(kbd, 200.0f, 20.0f);

  const auto first = kbd.cap_rect(0);
  const auto second = kbd.cap_rect(1);
  ST_CHECK(!first.is_empty());
  ST_CHECK(!second.is_empty());
  ST_CHECK(first.right() <= second.x + 0.01f);
  ST_CHECK(first.width > second.width);   // `Ctrl`(4 字符) 比 `S` 宽
}

/// 分隔符 `+` 会占位（键帽不重叠的前提下间距变大）。
ST_TEST(kbd_plus_separator_adds_space) {
  Fixture fixture;
  Kbd plain;
  plain.set_shortcut("Ctrl+S");
  fixture.layout(plain, 200.0f, 20.0f);
  const float plain_gap = plain.cap_rect(1).x - plain.cap_rect(0).right();

  Kbd with_plus;
  with_plus.set_shortcut("Ctrl+S");
  with_plus.set_separator(Kbd::Separator::Plus);
  fixture.layout(with_plus, 200.0f, 20.0f);
  const float plus_gap = with_plus.cap_rect(1).x - with_plus.cap_rect(0).right();

  ST_CHECK(plus_gap > plain_gap);
}

// ————————————————————————————————————————————————————————————————————————————
// Link
// ————————————————————————————————————————————————————————————————————————————

/// 下划线贴**基线**（不能按墨迹底画——那会穿过 `g`/`p` 的下伸部）。
ST_TEST(link_underline_sits_below_baseline) {
  Fixture fixture;
  Link link("jumping");
  fixture.layout(link, 200.0f, 20.0f);

  const auto text = link.text_rect();
  const auto underline = link.underline_rect();
  ST_CHECK(!text.is_empty());
  ST_CHECK(!underline.is_empty());
  // 下划线在文字块内（不是行盒底），且靠近文字块的垂直中部偏下。
  ST_CHECK(underline.y > text.y + text.height * 0.5f);
  ST_CHECK(underline.bottom() <= text.bottom());
  ST_CHECK(std::abs(underline.width - text.width) < 0.01f);
}

/// 下划线宽度跟**截断后**的文字走（不能还是原宽）。
ST_TEST(link_underline_follows_truncation) {
  Fixture fixture;
  Link link("a very long link text that must be truncated");
  fixture.layout(link, 80.0f, 20.0f);

  const auto text = link.text_rect();
  const auto underline = link.underline_rect();
  ST_CHECK(text.right() <= 80.0f + 0.01f);
  ST_CHECK(std::abs(underline.width - text.width) < 0.01f);
}

/// 关掉下划线后不再有下划线矩形。
ST_TEST(link_underline_can_be_disabled) {
  Fixture fixture;
  Link link("文字");
  link.set_underline(false);
  fixture.layout(link, 200.0f, 20.0f);
  ST_CHECK(link.underline_rect().is_empty());
}

/// `↗` 标记**不覆盖文字**，且文字让出它的位置。
ST_TEST(link_external_marker_does_not_overlap_text) {
  Fixture fixture;
  Link link("文档");
  link.set_external(true);
  fixture.layout(link, 200.0f, 20.0f);

  const auto text = link.text_rect();
  const auto marker = link.external_rect();
  ST_CHECK(!marker.is_empty());
  ST_CHECK(text.right() <= marker.x + 0.01f);

  // 没有 external 时无标记。
  Link plain("文档");
  fixture.layout(plain, 200.0f, 20.0f);
  ST_CHECK(plain.external_rect().is_empty());
}

/// 点击派发 `url`；`Enter` 同样可触发。
ST_TEST(link_click_and_enter_dispatch_url) {
  Fixture fixture;
  Link link("查看");
  link.set_url("https://example.com");
  fixture.layout(link, 200.0f, 20.0f);

  std::string reported;
  link.on_click = [&reported](std::string_view url) { reported = std::string(url); };
  fixture.click(link, link.text_rect().center());
  ST_CHECK(reported == "https://example.com");

  reported.clear();
  fixture.key(link, "Enter");
  ST_CHECK(reported == "https://example.com");
}

/// 属性面往返。
ST_TEST(link_property_roundtrip) {
  Link link;
  ST_CHECK(link.set_property("text", "文档"));
  ST_CHECK(link.set_property("url", "https://example.com/docs"));
  ST_CHECK(link.set_property("external", "true"));
  ST_CHECK(link.get_property("text").value_or("") == "文档");
  ST_CHECK(link.get_property("url").value_or("") == "https://example.com/docs");
  ST_CHECK(link.get_property("external").value_or("") == "true");
  ST_CHECK(link.external());
}
