/// **hint（悬停提示）计时必须是唤醒点语义，不是每帧拉起**（2026-11-04 修）。
///
/// ### 缺陷原形
///
/// 悬停一个带 hint 的控件后、气泡弹出前，有 450ms 的计时段。旧实现里
/// `collect_animation_requests` 对「hint 在计时」置 `animation_pending_ = true`——
/// 那是「连续帧」的语义：计时段内每 16ms 拉一帧，而这些帧**没有任何像素变化**
/// （气泡还没弹），每帧都走「damage 空 → 全量重绘」（headless 实测 6.9ms/帧、
/// 4K 软件窗口 20+7ms/帧）——鼠标悬停不动，CPU 却按 60fps 满转。
///
/// ### 修复语义（本用例钉住）
///
/// 1. **计时段不空转**：悬停静止后帧循环收敛（`needs_frame()` 为假）；
/// 2. **有唤醒点**：`next_wake_in_ms()` 返回有限的剩余毫秒（夹在 (0, delay] 内），
///    帧循环的空闲等待被夹到它——静止期间真正睡眠；
/// 3. **到期帧弹出气泡**：时间推进过延时后，`needs_frame()` 变真；那一帧
///    `paint_frame` 弹出共享气泡（`hint-bubble` 出现在树上且可见），且此后
///    帧循环再次收敛（气泡是静态的，不该常驻重绘）。
///
/// 三条对应「白转的帧没了 / 到点帧还在 / 弹出结果正确」——缺一条都会让
/// 这个修复在未来的重构里悄悄退化。

#include "st/test/test.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "st/raster/canvas.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

/// 最小文本端口（量宽/行高给常数值；按钮文案「确定」需要它）。
class BlankTextPort final : public st::ui::TextPort {
 public:
  static constexpr float kCellWidth{8.0F};
  static constexpr float kLineHeight{16.0F};

  [[nodiscard]] auto measure(std::string_view utf8, float) const -> st::math::Size override {
    return st::math::Size{width_of(utf8), kLineHeight};
  }
  [[nodiscard]] auto measure_width(std::string_view, float, st::text::FontRole) const
      -> float override {
    return kCellWidth * 4.0F;
  }
  [[nodiscard]] auto line_height(float) const -> float override { return kLineHeight; }
  [[nodiscard]] auto ascent(float) const -> float override { return 12.0F; }
  [[nodiscard]] auto descent(float) const -> float override { return 4.0F; }
  void draw(st::raster::Surface&, std::string_view, st::math::Point, float, st::math::Color,
            st::text::FontRole, float, bool) const override {}
  [[nodiscard]] auto ellipsize(std::string_view utf8, float, float) const -> std::string override {
    return std::string{utf8};
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float, float) const
      -> std::vector<std::string_view> override {
    return {utf8};
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float, float, std::size_t) const
      -> std::vector<std::string> override {
    return {std::string{utf8}};
  }

 private:
  [[nodiscard]] static auto width_of(std::string_view utf8) -> float {
    return kCellWidth * static_cast<float>(utf8.size());
  }
};

void expect(bool ok, std::string message) {
  ::st::test::Registry::instance().count_check();
  if (!ok) ::st::test::Registry::instance().record_failure(__FILE__, __LINE__, std::move(message));
}

}  // namespace

ST_TEST(hint_timing_uses_wake_point_not_frame_churn) {
  BlankTextPort port{};
  st::ui::UiRoot root{};
  root.set_theme(st::ui::Theme{st::ui::Theme::dark()});
  root.set_viewport(st::math::Size{300.0F, 200.0F});
  root.set_text_port(&port);

  auto owned = std::make_unique<st::ui::Button>("确定");
  st::ui::Button* button = owned.get();
  button->set_id("hint-btn");
  button->set_hint("提示文本");
  root.set_content(std::move(owned));
  root.layout(true);

  const auto context = st::ui::RenderContext{root.theme(), &port, 0.0};
  button->measure(context, st::ui::Constraints{});
  button->arrange(context, st::math::Rect{20.0F, 20.0F, 120.0F, 40.0F});

  st::raster::Canvas canvas{300, 200, 1.0F};
  double now = 1.0;
  root.set_time(now);
  root.paint_frame(canvas);
  root.clear_dirty();

  // 悬停到按钮上（提示计时从这里开始——单调钟起点）。
  st::ui::Event move{};
  move.kind = st::ui::EventKind::MouseMove;
  move.position = st::math::Point{60.0F, 40.0F};
  (void)root.dispatch(move);

  // 悬浮过渡是几帧动画：跑到收敛（过渡结束、计时段开始）。
  for (int i = 0; i < 40 && root.dirty(); ++i) {
    now += 0.016;
    root.set_time(now);
    root.paint_frame(canvas);
    root.clear_dirty();
  }
  expect(button->hovered(), "夹具失败：按钮未进入悬浮态");

  // ── 判据 1：计时段不空转 ──
  // 悬停静止、气泡未弹：此刻不该有任何「还要帧」的理由。
  expect(!root.dirty(),
         "计时段内 needs_frame 为真——hint 仍在用 animation_pending 拉帧（每帧全量白画）");

  // ── 判据 2：有唤醒点 ──
  const double wake_in_ms = root.next_wake_in_ms();
  expect(wake_in_ms > 0.0 && wake_in_ms <= static_cast<double>(root.hint_delay_ms()) + 50.0,
         std::format("唤醒点缺失或异常：next_wake_in_ms={}（应在 (0, hint_delay] 内）",
                     wake_in_ms));

  // ── 判据 3：到期帧弹出气泡 ──
  // 计时用单调墙钟：真睡到延时过后（测试默认 280ms，睡 400ms 足够）。
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  expect(root.dirty(), "到点后 needs_frame 仍为假——hint 到期帧没人拉（气泡永不弹出）");
  now += 0.5;
  root.set_time(now);
  (void)root.paint_frame(canvas);
  root.clear_dirty();
  const st::ui::Element* bubble = root.find("hint-bubble");
  expect(bubble != nullptr, "到期帧没有弹出共享气泡（hint-bubble 不在树上）");
  expect(bubble != nullptr && bubble->visible(), "气泡在树上但不可见");

  // 弹出后收敛：气泡是静态的，不该常驻重绘。
  // 先过粘滞期（kHintMinVisibleMs=900ms：显示初期需要帧来检查何时可收），
  // 再跑到帧循环收敛。
  for (int i = 0; i < 40 && root.dirty(); ++i) {
    now += 0.016;
    root.set_time(now);
    root.paint_frame(canvas);
    root.clear_dirty();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  root.set_time(now);
  // 粘滞期后、未失联（鼠标仍在按钮上）：允许有待处理帧（收起检查），
  // 但把它们消耗完后必须收敛。
  for (int i = 0; i < 8 && root.dirty(); ++i) {
    root.set_time(now);
    root.paint_frame(canvas);
    root.clear_dirty();
  }
  expect(!root.dirty(), "气泡弹出并稳定后帧循环不收敛——仍在常驻重绘");
}
