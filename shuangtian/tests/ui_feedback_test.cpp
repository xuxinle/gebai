#include "st/test/test.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/components/overlay.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

/// 等宽假文本端口（8px/码点，行高 1.45×字号）：度量可复现，绘制只记录不落地。
class FeedbackTestTextPort final : public st::ui::TextPort {
 public:
  static constexpr float kAdvance{8.0f};

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return st::math::Size{measure_width(utf8, size), line_height(size)};
  }

  [[nodiscard]] auto measure_width(std::string_view utf8, float size,
                     st::text::FontRole role = st::text::FontRole::Proportional) const
      -> float override {
      (void)role;  // 桩：等宽与比例同宽，无需区分
    (void)size;
    return kAdvance * static_cast<float>(st::utf8_length(utf8));
  }

  [[nodiscard]] auto line_height(float size) const -> float override { return size * 1.45f; }

  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float size,
            st::math::Color color,
            st::text::FontRole role = st::text::FontRole::Proportional,
            float embolden = 0.0f, bool = false) const override {
    (void)role;      // 桩
    (void)embolden;  // 桩（桩不模拟字重：断言只关心布局与颜色）
    (void)canvas;
    (void)origin;
    (void)size;
    (void)color;
    drawn.emplace_back(utf8);
  }

  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    if (max_width <= 0.0f) return {};
    if (measure_width(utf8, size) <= max_width) return std::string(utf8);
    const auto capacity = static_cast<std::size_t>(max_width / kAdvance);
    if (capacity <= 1U) return std::string("…");
    return std::string(st::utf8_slice(utf8, 0, capacity - 1U)) + "…";
  }

  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    (void)size;
    (void)max_width;
    return {utf8};
  }

  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const
      -> std::vector<std::string> override {
    (void)size;
    (void)max_width;
    (void)max_lines;
    return {std::string(utf8)};
  }

  mutable std::vector<std::string> drawn{};
};

/// 直通 alpha 合成（预期像素用）。
[[nodiscard]] auto composite(st::math::Color base, st::math::Color over) -> st::math::Color {
  const float alpha = static_cast<float>(over.a) / 255.0f;
  const auto mix = [alpha](std::uint8_t bottom, std::uint8_t top) -> std::uint8_t {
    const float value =
        static_cast<float>(bottom) * (1.0f - alpha) + static_cast<float>(top) * alpha;
    return static_cast<std::uint8_t>(value + 0.5f);
  };
  return st::math::Color{mix(base.r, over.r), mix(base.g, over.g), mix(base.b, over.b), 255};
}

[[nodiscard]] auto near_color(st::math::Color actual, st::math::Color expected, int tolerance)
    -> bool {
  const auto diff = [](std::uint8_t left, std::uint8_t right) -> int {
    return std::abs(static_cast<int>(left) - static_cast<int>(right));
  };
  return diff(actual.r, expected.r) <= tolerance && diff(actual.g, expected.g) <= tolerance &&
         diff(actual.b, expected.b) <= tolerance;
}

/// 两画布逐像素比较（尺寸须相同）。
[[nodiscard]] auto same_pixels(const st::raster::Canvas& left, const st::raster::Canvas& right)
    -> bool {
  if (left.physical_width() != right.physical_width() ||
      left.physical_height() != right.physical_height()) {
    return false;
  }
  for (int y = 0; y < left.physical_height(); ++y) {
    for (int x = 0; x < left.physical_width(); ++x) {
      if (!(left.pixel_at(x, y) == right.pixel_at(x, y))) return false;
    }
  }
  return true;
}

}  // namespace

// —— ② ProgressBar：50% 时左半填充 / 右半轨道 ——

ST_TEST(ui_progressbar_fill_and_track) {
  st::ui::Theme theme = st::ui::Theme::light();
  FeedbackTestTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::ProgressBar bar;
  bar.set_value(1.5f);
  ST_CHECK_NEAR(bar.value(), 1.0f, 0.001f);
  bar.set_value(-1.0f);
  ST_CHECK_NEAR(bar.value(), 0.0f, 0.001f);
  bar.set_value(0.5f);
  ST_CHECK_NEAR(bar.value(), 0.5f, 0.001f);
  bar.apply_theme(theme);

  const st::ui::Constraints constraints;
  bar.measure(context, constraints);
  ST_CHECK_NEAR(bar.measured_size().height, 6.0f, 0.01f);
  bar.arrange(context, st::math::Rect{0.0f, 0.0f, 200.0f, 6.0f});

  st::raster::Canvas canvas(200, 6);
  canvas.clear(theme.colors().surface);
  bar.paint(context, canvas);

  const st::math::Color left = canvas.pixel_at_point(st::math::Point{50.0f, 3.0f});
  const st::math::Color right = canvas.pixel_at_point(st::math::Point{150.0f, 3.0f});
  // 右半是轨道色（surface_sunken），左半是渐变填充（primary → primary_hover 的中段）
  ST_CHECK(right == theme.colors().surface_sunken);
  const st::math::Color expected =
      st::math::lerp(theme.colors().primary, theme.colors().primary_hover, 0.505f);
  ST_CHECK(near_color(left, expected, 4));
  ST_CHECK(!(left == right));
  ST_CHECK(near_color(left, theme.colors().primary, 12));

  // 语义：value = 百分比字符串
  ST_CHECK_EQ(bar.semantics_value(), std::string{"50%"});
  ST_CHECK_EQ(std::string{st::ui::to_string(bar.role())}, std::string{"progress_bar"});
  ST_CHECK_EQ(bar.get_property("value").value_or(""), std::string{"0.5"});
  ST_CHECK(bar.set_property("value", "25%"));
  ST_CHECK_NEAR(bar.value(), 0.25f, 0.001f);
  ST_CHECK_EQ(bar.semantics_value(), std::string{"25%"});
}

// —— ③ Spinner：动画由 time_seconds 驱动（像素证明）——

ST_TEST(ui_spinner_animation) {
  st::ui::Theme theme = st::ui::Theme::light();
  FeedbackTestTextPort port;
  const st::ui::RenderContext at0{theme, &port, 0.0};
  const st::ui::RenderContext at03{theme, &port, 0.3};
  const st::ui::RenderContext at12{theme, &port, 1.2};

  st::ui::Spinner spinner;
  spinner.apply_theme(theme);
  const st::ui::Constraints constraints;
  spinner.measure(at0, constraints);
  ST_CHECK_NEAR(spinner.measured_size().width, 18.0f, 0.01f);
  ST_CHECK_NEAR(spinner.diameter(), 18.0f, 0.01f);
  spinner.arrange(at0, st::math::Rect{0.0f, 0.0f, 18.0f, 18.0f});

  ST_CHECK_NEAR(spinner.phase_degrees(0.0), 0.0f, 0.01f);
  ST_CHECK_NEAR(spinner.phase_degrees(0.3), 90.0f, 0.01f);
  ST_CHECK_NEAR(spinner.phase_degrees(0.6), 180.0f, 0.01f);

  st::raster::Canvas frame0(18, 18);
  st::raster::Canvas frame03(18, 18);
  st::raster::Canvas frame12(18, 18);
  frame0.clear(st::math::Color{0, 0, 0, 0});
  frame03.clear(st::math::Color{0, 0, 0, 0});
  frame12.clear(st::math::Color{0, 0, 0, 0});
  spinner.paint(at0, frame0);
  spinner.paint(at03, frame03);
  spinner.paint(at12, frame12);

  // 画了东西（3/4 弧 + 2px 粗）
  ST_CHECK(!frame0.content_bounds().is_empty());
  // 不同时刻的像素不同 → 动画由 context.time_seconds 驱动
  ST_CHECK(!same_pixels(frame0, frame03));
  // 同一相位（一整圈 1.2s）像素完全一致
  ST_CHECK(same_pixels(frame0, frame12));
  ST_CHECK_EQ(spinner.semantics_text(), std::string{"loading"});
  ST_CHECK_EQ(spinner.get_property("size").value_or(""), std::string{"18"});
}

// —— ⑤ Badge / Chip / Avatar：语义与尺寸 + 形状像素 ——

ST_TEST(ui_badge_chip_avatar) {
  st::ui::Theme theme = st::ui::Theme::light();
  FeedbackTestTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};
  const st::ui::Constraints constraints;

  // —— Badge ——
  st::ui::Badge badge("NEW", st::ui::Tone::Success);
  badge.apply_theme(theme);
  badge.measure(context, constraints);
  ST_CHECK_NEAR(badge.measured_size().width, 40.0f, 0.01f);  // 3 码点 ×8 + 2×8
  ST_CHECK_NEAR(badge.measured_size().height, 20.0f, 0.01f);
  ST_CHECK_EQ(badge.semantics_text(), std::string{"NEW"});
  ST_CHECK_EQ(badge.semantics_value(), std::string{"success"});
  ST_CHECK_EQ(std::string{st::ui::to_string(badge.role())}, std::string{"text"});
  ST_CHECK_EQ(badge.get_property("tone").value_or(""), std::string{"success"});
  badge.arrange(context, st::math::Rect{0.0f, 0.0f, 40.0f, 20.0f});

  st::raster::Canvas badge_canvas(40, 20);
  badge_canvas.clear(theme.colors().bg);
  badge.paint(context, badge_canvas);
  const st::math::Color pill = composite(theme.colors().bg, st::ui::tone_soft_color(theme, st::ui::Tone::Success));
  ST_CHECK(near_color(badge_canvas.pixel_at_point(st::math::Point{20.0f, 10.0f}), pill, 3));
  // 胶囊外的角保持底色
  ST_CHECK(badge_canvas.pixel_at_point(st::math::Point{1.0f, 1.0f}) == theme.colors().bg);

  // —— Chip ——
  st::ui::Chip chip("筛选", true);
  chip.set_dot(true);
  chip.apply_theme(theme);
  chip.measure(context, constraints);
  ST_CHECK_NEAR(chip.measured_size().width, 74.0f, 0.01f);  // 12 + (6+6) + 16 + (6+16) + 12
  ST_CHECK_NEAR(chip.measured_size().height, 26.0f, 0.01f);
  chip.arrange(context, st::math::Rect{0.0f, 0.0f, 74.0f, 26.0f});
  ST_CHECK_NEAR(chip.text_area().x, 24.0f, 0.01f);
  ST_CHECK_NEAR(chip.close_area().x, 46.0f, 0.01f);
  ST_CHECK_NEAR(chip.close_area().width, 16.0f, 0.01f);
  ST_CHECK_NEAR(chip.close_area().height, 16.0f, 0.01f);
  ST_CHECK_EQ(chip.semantics_text(), std::string{"筛选"});
  ST_CHECK_EQ(std::string{st::ui::to_string(chip.role())}, std::string{"button"});

  bool closed = false;
  bool chip_clicked = false;
  chip.on_close = [&closed]() { closed = true; };
  chip.on_click = [&chip_clicked]() { chip_clicked = true; };
  st::ui::Event close_press;
  close_press.kind = st::ui::EventKind::MouseDown;
  close_press.position = st::math::Point{54.0f, 13.0f};  // 关闭 × 命中区中心
  ST_CHECK(chip.on_event(context, close_press));
  ST_CHECK(closed);
  ST_CHECK(!chip_clicked);

  closed = false;
  st::ui::Event body_click;
  body_click.kind = st::ui::EventKind::Click;
  body_click.position = st::math::Point{30.0f, 13.0f};  // 文本区
  ST_CHECK(chip.on_event(context, body_click));
  ST_CHECK(chip_clicked);
  ST_CHECK(!closed);

  st::raster::Canvas chip_canvas(74, 26);
  chip_canvas.clear(st::math::Color{0, 0, 0, 0});
  chip.paint(context, chip_canvas);
  // 前导圆点 = tone_color（不透明，像素精确）
  ST_CHECK(chip_canvas.pixel_at_point(st::math::Point{15.0f, 13.0f}) ==
           st::ui::tone_color(theme, st::ui::Tone::Default));
  ST_CHECK_EQ(chip.get_property("dot").value_or(""), std::string{"true"});
  ST_CHECK_EQ(chip.get_property("closable").value_or(""), std::string{"true"});

  // —— Avatar ——
  st::ui::Avatar avatar("Ada Lovelace", st::ui::Avatar::kLarge);
  avatar.apply_theme(theme);
  avatar.measure(context, constraints);
  ST_CHECK_NEAR(avatar.measured_size().width, 48.0f, 0.01f);
  ST_CHECK_NEAR(avatar.measured_size().height, 48.0f, 0.01f);
  ST_CHECK_EQ(avatar.initials(), std::string{"A"});
  ST_CHECK_EQ(avatar.semantics_value(), std::string{"Ada Lovelace"});
  ST_CHECK_EQ(avatar.semantics_text(), std::string{"A"});
  ST_CHECK_EQ(std::string{st::ui::to_string(avatar.role())}, std::string{"image"});
  avatar.arrange(context, st::math::Rect{0.0f, 0.0f, 48.0f, 48.0f});

  st::raster::Canvas avatar_canvas(48, 48);
  avatar_canvas.clear(theme.colors().bg);
  avatar.paint(context, avatar_canvas);
  ST_CHECK(avatar_canvas.pixel_at_point(st::math::Point{24.0f, 24.0f}) ==
           theme.colors().primary_soft);
  ST_CHECK(avatar_canvas.pixel_at_point(st::math::Point{1.0f, 1.0f}) == theme.colors().bg);
  avatar.set_size(st::ui::Avatar::kSmall);
  avatar.measure(context, constraints);
  ST_CHECK_NEAR(avatar.measured_size().width, 28.0f, 0.01f);
  ST_CHECK_EQ(avatar.get_property("initials").value_or(""), std::string{"A"});

  st::ui::Avatar chinese("张三");
  ST_CHECK_EQ(chinese.initials(), std::string{"张"});
}

// —— Tooltip：目标上方定位 + 显隐 ——

ST_TEST(ui_tooltip_target_and_visibility) {
  st::ui::Theme theme = st::ui::Theme::light();
  FeedbackTestTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};
  const st::ui::Constraints constraints;

  st::ui::Tooltip tip("保存文件");
  tip.apply_theme(theme);
  tip.set_target_rect(st::math::Rect{100.0f, 200.0f, 60.0f, 24.0f});
  tip.set_active(true);
  tip.measure(context, constraints);
  ST_CHECK_NEAR(tip.measured_size().width, 48.0f, 0.01f);
  // 字号取自主题（`Tooltip` 用 `metrics.font_xs`）——断言跟着主题走，
  // 不写死数字：字号阶梯是设计 token，写死会让“调字号”变成改测试。
  ST_CHECK_NEAR(tip.measured_size().height, theme.metrics().font_xs * 1.45f + 8.0f, 0.01f);

  tip.arrange(context, st::math::Rect{0.0f, 0.0f, 0.0f, 0.0f});
  ST_CHECK_NEAR(tip.bounds().center().x, 130.0f, 0.01f);
  ST_CHECK_NEAR(tip.bounds().bottom(), 194.0f, 0.01f);  // 目标上沿 - 6px 间距
  ST_CHECK(tip.hit_test(st::math::Point{130.0f, 180.0f}));
  ST_CHECK_EQ(std::string{st::ui::to_string(tip.role())}, std::string{"tooltip"});

  st::raster::Canvas shown(300, 260);
  shown.clear(st::math::Color{0, 0, 0, 0});
  tip.paint(context, shown);
  ST_CHECK(!shown.content_bounds().is_empty());

  st::raster::Canvas hidden(300, 260);
  hidden.clear(st::math::Color{0, 0, 0, 0});
  tip.set_active(false);
  tip.paint(context, hidden);
  ST_CHECK(hidden.content_bounds().is_empty());
  ST_CHECK(!tip.hit_test(st::math::Point{130.0f, 180.0f}));
  ST_CHECK_EQ(tip.get_property("active").value_or(""), std::string{"false"});
}

// —— ④ Dialog：遮罩 + Esc + 点击遮罩外关闭 ——

ST_TEST(ui_dialog_scrim_and_dismiss) {
  st::ui::Theme theme = st::ui::Theme::light();
  st::ui::UiRoot root;
  root.set_theme(theme);
  root.set_viewport(st::math::Size{900.0f, 600.0f});
  root.set_text_port(nullptr);  // ⑥ 空 TextPort 路径

  auto dialog = std::make_unique<st::ui::Dialog>("确认删除", "该操作不可撤销，请谨慎操作。");
  dialog->set_actions({"取消", "删除"});
  dialog->set_viewport_rect(st::math::Rect{0.0f, 0.0f, 900.0f, 600.0f});
  root.set_content(std::make_unique<st::ui::Panel>());  // 对话框叠在日常内容之上
  bool acted = false;
  std::size_t acted_index = 99U;
  bool dismissed = false;
  st::ui::Dialog* raw = dialog.get();
  raw->on_action = [&acted, &acted_index](std::size_t index) {
    acted = true;
    acted_index = index;
  };
  raw->on_dismiss = [&dismissed]() { dismissed = true; };
  root.add_overlay(std::move(dialog));
  root.layout(true);

  // 卡片：宽 420~520、水平垂直居中
  const st::math::Rect card = raw->card_rect();
  ST_CHECK(card.width >= st::ui::Dialog::kMinWidth && card.width <= st::ui::Dialog::kMaxWidth);
  ST_CHECK_NEAR(card.center().x, 450.0f, 0.5f);
  ST_CHECK_NEAR(card.center().y, 300.0f, 0.5f);
  ST_CHECK_EQ(raw->action_count(), 2U);
  // 按钮行：右对齐、末项（主按钮）贴卡片右内边距
  const st::math::Rect last = raw->action_rect(1U);
  const st::math::Rect first = raw->action_rect(0U);
  ST_CHECK(last.width > 0.0f);
  ST_CHECK_NEAR(last.right(), card.right() - st::ui::Dialog::kPadding, 0.5f);
  ST_CHECK(first.right() <= last.x + 0.01f);

  // 遮罩：角落是 bg 与 overlay 的合成，卡片中心是 surface
  st::raster::Canvas canvas(900, 600);
  canvas.clear(theme.colors().bg);
  root.paint(canvas);
  const st::math::Color corner = canvas.pixel_at_point(st::math::Point{5.0f, 5.0f});
  ST_CHECK(!(corner == theme.colors().bg));
  ST_CHECK(near_color(corner, composite(theme.colors().bg, theme.colors().overlay), 4));
  ST_CHECK(canvas.pixel_at_point(card.center()) == theme.colors().surface);
  // 卡片外仍是遮罩
  const st::math::Color scrim = composite(theme.colors().bg, theme.colors().overlay);
  ST_CHECK(near_color(canvas.pixel_at_point(st::math::Point{20.0f, 580.0f}), scrim, 4));

  // Esc → on_dismiss
  root.set_focus(raw);
  ST_CHECK_EQ(std::string{st::ui::to_string(raw->role())}, std::string{"dialog"});
  ST_CHECK_EQ(raw->semantics_text(), std::string{"确认删除"});
  st::ui::Event key;
  key.kind = st::ui::EventKind::KeyDown;
  key.key = "Escape";
  ST_CHECK(root.dispatch(key));
  ST_CHECK(dismissed);
  ST_CHECK(key.handled);

  // 点击遮罩（卡片外）→ on_dismiss
  dismissed = false;
  st::ui::Event outside;
  outside.kind = st::ui::EventKind::Click;
  outside.position = st::math::Point{10.0f, 10.0f};
  ST_CHECK(root.dispatch(outside));
  ST_CHECK(dismissed);

  // 点击卡片内部不动关闭
  dismissed = false;
  st::ui::Event inside;
  inside.kind = st::ui::EventKind::Click;
  inside.position = st::math::Point{card.x + 5.0f, card.y + 5.0f};
  (void)root.dispatch(inside);
  ST_CHECK(!dismissed);

  // 属性面：actions 往返
  ST_CHECK(raw->set_property("actions", "取消|删除"));
  ST_CHECK_EQ(raw->get_property("actions").value_or(""), std::string{"取消|删除"});
  ST_CHECK(raw->invoke_action("action", "1"));
  ST_CHECK(acted);
  ST_CHECK_EQ(acted_index, 1U);
  ST_CHECK(raw->invoke_action("dismiss", ""));
  ST_CHECK(dismissed);
}

// —— Toast：色条 + 尺寸 + 语义 ——

ST_TEST(ui_toast_tone_bar) {
  st::ui::Theme theme = st::ui::Theme::light();
  FeedbackTestTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};
  const st::ui::Constraints constraints;

  auto toast = st::ui::Toast::make("已保存到本地", st::ui::Tone::Success);
  toast->apply_theme(theme);
  toast->measure(context, constraints);
  ST_CHECK_NEAR(toast->measured_size().height, 40.0f, 0.01f);
  ST_CHECK_NEAR(toast->measured_size().width, 6.0f * 8.0f + 4.0f + 14.0f + 14.0f, 0.01f);
  toast->arrange(context, st::math::Rect{0.0f, 0.0f, 80.0f, 40.0f});

  st::raster::Canvas canvas(80, 40);
  canvas.clear(st::math::Color{0, 0, 0, 0});
  toast->paint(context, canvas);
  ST_CHECK(canvas.pixel_at_point(st::math::Point{2.0f, 20.0f}) ==
           st::ui::tone_color(theme, st::ui::Tone::Success));
  ST_CHECK(canvas.pixel_at_point(st::math::Point{40.0f, 20.0f}) == theme.colors().surface);
  ST_CHECK_EQ(toast->semantics_text(), std::string{"已保存到本地"});
  ST_CHECK_EQ(toast->semantics_value(), std::string{"success"});
  ST_CHECK_EQ(std::string{st::ui::to_string(toast->role())}, std::string{"panel"});
  ST_CHECK_EQ(toast->get_property("tone").value_or(""), std::string{"success"});
  ST_CHECK(toast->set_property("tone", "danger"));
  ST_CHECK_EQ(std::string{st::ui::to_string(toast->role())}, std::string{"panel"});
}

// —— Toast 自动消失（v0.1.5：帧时间轴驱动 + on_dismiss + 属性面）——

ST_TEST(ui_toast_auto_dismiss) {
  st::ui::Theme theme = st::ui::Theme::light();
  FeedbackTestTextPort port;
  const st::ui::Constraints constraints;

  auto toast = st::ui::Toast::make("已保存", st::ui::Tone::Success);
  toast->apply_theme(theme);
  toast->set_auto_dismiss_ms(1000.0);
  toast->measure(st::ui::RenderContext{theme, &port, 0.0}, constraints);
  toast->arrange(st::ui::RenderContext{theme, &port, 0.0},
                 st::math::Rect{0.0f, 0.0f, 80.0f, 40.0f});

  // 属性面读写
  ST_CHECK_EQ(toast->get_property("auto_dismiss_ms").value_or(""), std::string{"1000"});
  ST_CHECK_EQ(toast->get_property("expired").value_or(""), std::string{"false"});
  ST_CHECK(toast->set_property("auto_dismiss_ms", "500"));
  ST_CHECK_NEAR(toast->auto_dismiss_ms(), 500.0, 0.01);
  ST_CHECK(!toast->set_property("auto_dismiss_ms", "-3"));  // 负数拒绝
  ST_CHECK(toast->set_property("auto_dismiss_ms", "1000"));

  st::raster::Canvas canvas(80, 40);
  canvas.clear(st::math::Color{0, 0, 0, 0});

  // 静态时间轴（now 停滞）：首帧起算但不误触发
  const double now = 10.0;
  st::ui::RenderContext t0{theme, &port, now};
  toast->paint(t0, canvas);
  ST_CHECK(!toast->expired());
  toast->paint(t0, canvas);  // 同时刻重复绘制：仍不触发
  ST_CHECK(!toast->expired());

  // 时间推进到 599ms：未到期，仍绘制
  st::ui::RenderContext t1{theme, &port, now + 0.599};
  toast->paint(t1, canvas);
  ST_CHECK(!toast->expired());

  // 时间推进到 1000ms：到期 → on_dismiss 触发，不再绘制
  int dismissed = 0;
  toast->on_dismiss = [&dismissed]() { ++dismissed; };
  st::raster::Canvas fresh(80, 40);
  fresh.clear(st::math::Color{255, 0, 0, 255});  // 与 Toast 不同的底色，验证不再绘制
  st::ui::RenderContext t2{theme, &port, now + 1.000};
  toast->paint(t2, fresh);
  ST_CHECK(toast->expired());
  ST_CHECK_EQ(dismissed, 1);
  const st::math::Color sentinel{255, 0, 0, 255};
  ST_CHECK(fresh.pixel_at_point(st::math::Point{40.0f, 20.0f}) == sentinel);  // 未被覆盖：到期帧不再绘制
  // 到期后重复绘制：不会重复触发
  toast->paint(t2, fresh);
  ST_CHECK_EQ(dismissed, 1);

  // 常驻模式（0ms）：永不触发
  auto stay = st::ui::Toast::make("常驻", st::ui::Tone::Default);
  stay->apply_theme(theme);
  stay->measure(st::ui::RenderContext{theme, &port, 0.0}, constraints);
  stay->arrange(st::ui::RenderContext{theme, &port, 0.0},
                st::math::Rect{0.0f, 0.0f, 80.0f, 40.0f});
  int stay_dismissed = 0;
  stay->on_dismiss = [&stay_dismissed]() { ++stay_dismissed; };
  const st::ui::RenderContext far{theme, &port, 100000.0};
  stay->paint(far, canvas);
  stay->paint(far, canvas);
  ST_CHECK(!stay->expired());
  ST_CHECK_EQ(stay_dismissed, 0);
}

// —— ⑥ 空 TextPort：全部反馈组件不崩溃且形状仍落盘 ——

ST_TEST(ui_feedback_with_null_text_port) {
  st::ui::Theme theme = st::ui::Theme::dark();
  const st::ui::RenderContext context{theme, nullptr, 0.5};  // text == nullptr
  const st::ui::Constraints constraints;

  st::ui::ProgressBar bar;
  bar.set_value(0.5f);
  bar.apply_theme(theme);
  bar.measure(context, constraints);
  bar.arrange(context, st::math::Rect{0.0f, 0.0f, 120.0f, 6.0f});
  st::raster::Canvas bar_canvas(120, 6);
  bar_canvas.clear(theme.colors().bg);
  bar.paint(context, bar_canvas);
  ST_CHECK(bar_canvas.pixel_at_point(st::math::Point{100.0f, 3.0f}) ==
           theme.colors().surface_sunken);

  st::ui::Spinner spinner;
  spinner.apply_theme(theme);
  spinner.measure(context, constraints);
  spinner.arrange(context, st::math::Rect{0.0f, 0.0f, 18.0f, 18.0f});
  st::raster::Canvas spinner_canvas(18, 18);
  spinner_canvas.clear(st::math::Color{0, 0, 0, 0});
  spinner.paint(context, spinner_canvas);
  ST_CHECK(!spinner_canvas.content_bounds().is_empty());

  st::ui::Badge badge("beta", st::ui::Tone::Primary);
  badge.apply_theme(theme);
  badge.measure(context, constraints);
  badge.arrange(context, st::math::Rect{0.0f, 0.0f, badge.measured_size().width,
                                        badge.measured_size().height});
  st::raster::Canvas badge_canvas(64, 20);
  badge_canvas.clear(st::math::Color{0, 0, 0, 0});
  badge.paint(context, badge_canvas);
  ST_CHECK(!badge_canvas.content_bounds().is_empty());

  st::ui::Chip chip("标签", true);
  chip.set_dot(true);
  chip.apply_theme(theme);
  chip.measure(context, constraints);
  chip.arrange(context, st::math::Rect{0.0f, 0.0f, chip.measured_size().width,
                                       chip.measured_size().height});
  st::raster::Canvas chip_canvas(96, 26);
  chip_canvas.clear(st::math::Color{0, 0, 0, 0});
  chip.paint(context, chip_canvas);
  ST_CHECK(!chip_canvas.content_bounds().is_empty());

  st::ui::Avatar avatar("Zed");
  avatar.apply_theme(theme);
  avatar.measure(context, constraints);
  avatar.arrange(context, st::math::Rect{0.0f, 0.0f, 36.0f, 36.0f});
  st::raster::Canvas avatar_canvas(36, 36);
  avatar_canvas.clear(st::math::Color{0, 0, 0, 0});
  avatar.paint(context, avatar_canvas);
  ST_CHECK(avatar_canvas.pixel_at_point(st::math::Point{18.0f, 18.0f}) ==
           theme.colors().primary_soft);

  st::ui::Tooltip tip("");
  tip.apply_theme(theme);
  tip.set_active(true);
  tip.set_target_rect(st::math::Rect{10.0f, 40.0f, 20.0f, 10.0f});
  tip.measure(context, constraints);
  tip.arrange(context, st::math::Rect{0.0f, 0.0f, 0.0f, 0.0f});
  st::raster::Canvas tip_canvas(80, 60);
  tip_canvas.clear(st::math::Color{0, 0, 0, 0});
  tip.paint(context, tip_canvas);  // 空文本 + 空端口：不崩溃

  auto toast = st::ui::Toast::make("", st::ui::Tone::Warning);
  toast->apply_theme(theme);
  toast->measure(context, constraints);
  toast->arrange(context, st::math::Rect{0.0f, 0.0f, 60.0f, 40.0f});
  st::raster::Canvas toast_canvas(60, 40);
  toast_canvas.clear(st::math::Color{0, 0, 0, 0});
  toast->paint(context, toast_canvas);
  ST_CHECK(!toast_canvas.content_bounds().is_empty());

  // 对话框：空端口下遮罩与卡片仍落盘
  auto dialog = std::make_unique<st::ui::Dialog>("标题", "正文");
  dialog->set_actions({"确定"});
  dialog->set_viewport_rect(st::math::Rect{0.0f, 0.0f, 200.0f, 160.0f});
  dialog->apply_theme(theme);
  dialog->measure(context, constraints);
  dialog->arrange(context, st::math::Rect{0.0f, 0.0f, 200.0f, 160.0f});
  st::raster::Canvas dialog_canvas(200, 160);
  dialog_canvas.clear(theme.colors().bg);
  dialog->paint(context, dialog_canvas);
  ST_CHECK(dialog_canvas.pixel_at_point(st::math::Point{2.0f, 2.0f}) != theme.colors().bg);
  ST_CHECK(dialog_canvas.pixel_at_point(dialog->card_rect().center()) == theme.colors().surface);
}
