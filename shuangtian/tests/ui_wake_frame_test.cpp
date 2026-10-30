/// **定时唤醒链必须在唤醒帧上闭合**（缺陷：闪烁/Toast 在第二帧就冻住）。
///
/// ### 缺陷原形（2026-10-29 引入，同日暴露）
///
/// 把光标闪烁从「每帧 `request_animation()` 连续续帧」改成「登记下一个翻转点
/// `request_wake_at`」之后，`needs_frame()` 增加了一条 `wake_due` 判据——
/// 到点就拉起一帧。但 `paint_frame` 开头的**空帧早退**只看损坏区：
///
/// ```
/// if (!full_flag && clipped_damage.is_empty()) {   // 唤醒帧恰好没有帧间 damage
///   last_frame_presentable_ = false;
///   collect_animation_requests();
///   return false;                                   // ← 根本没画
/// }
/// ```
///
/// 而唤醒点是「消费-重立」协议：`collect_animation_requests` 汇总时**清掉元素侧**，
/// 新的唤醒点只能在 `paint` 里重新登记。于是：
///
/// ```
/// 唤醒帧不绘制 → 元素没机会重立 → clear_dirty 再清掉到期的 wake_at_
///   ⇒ 既无唤醒点、也无损坏区 ⇒ 光标/Toast **永久冻结**
/// ```
///
/// ### 判据（三道，缺一条都抓不住）
///
/// 1. 聚焦静止时帧循环**收敛**（不烧帧预算）——每帧续帧的老缺陷；
/// 2. 有**有限的正值**唤醒点（闪烁仍会继续）；
/// 3. **唤醒帧真的画了**，且画完之后**又有了新的唤醒点**——这条正是缺陷原形。
///
/// 第 3 条用 `last_frame_presentable()` + 连续两轮"推到唤醒点"来验：
/// 只验一轮的话，"第一轮碰巧有其它 damage、第二轮才断"会被判绿。

#include "st/test/test.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/text/ansi_screen.hpp"
#include "st/ui/components/terminal.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Terminal;

/// 最小文本端口：绘制路径需要它来量格宽/行高（不需要真字形）。
class BlankTextPort final : public st::ui::TextPort {
 public:
  static constexpr float kCellWidth{8.0F};
  static constexpr float kLineHeight{16.0F};
  static constexpr float kAscent{12.0F};
  mutable int draws{0};

  [[nodiscard]] auto measure(std::string_view utf8, float) const -> st::math::Size override {
    return st::math::Size{width_of(utf8), kLineHeight};
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float,
                                   st::text::FontRole) const -> float override {
    return width_of(utf8);
  }
  [[nodiscard]] auto line_height(float) const -> float override { return kLineHeight; }
  [[nodiscard]] auto ascent(float) const -> float override { return kAscent; }
  [[nodiscard]] auto descent(float) const -> float override { return kLineHeight - kAscent; }
  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float,
            st::math::Color color, st::text::FontRole, float, bool) const override {
    ++draws;
    canvas.fill_rect(st::math::Rect{origin.x, origin.y, width_of(utf8), kAscent},
                     st::raster::Paint::solid(color));
  }
  [[nodiscard]] auto ellipsize(std::string_view, float, float) const -> std::string override {
    return {};
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

/// 带详情的断言（`ST_CHECK` 的宏消息只有源码文本，说不出为啥失败）。
void expect(bool ok, std::string message) {
  ::st::test::Registry::instance().count_check();
  if (!ok) ::st::test::Registry::instance().record_failure(__FILE__, __LINE__, std::move(message));
}

}  // namespace

ST_TEST(wake_frame_paints_and_rearms_the_next_wake) {
  if (!st::process::PtySession::supported()) return;
  BlankTextPort port{};
  st::ui::UiRoot root{};
  root.set_theme(st::ui::Theme{st::ui::Theme::dark()});
  root.set_viewport(st::math::Size{800.0F, 400.0F});
  root.set_text_port(&port);

  auto owned = std::make_unique<Terminal>();
  Terminal* terminal = owned.get();
  terminal->set_id("term-wake");
  root.set_content(std::move(owned));
  root.layout(true);

  terminal->open_shell();
  st::raster::Canvas canvas{800, 400, 1.0F};

  // 跑到有内容（提示符出来）
  for (int i = 0; i < 60 && port.draws == 0; ++i) {
    terminal->pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    root.layout(false);
    root.set_time(static_cast<double>(i) * 0.03);
    root.paint_frame(canvas);
    root.clear_dirty();
  }
  expect(port.draws > 0, "终端一帧都没画（夹具没跑起来）");
  expect(root.set_focus(terminal), "无法把焦点给终端");
  expect(terminal->focused(), "焦点没落在终端上");

  // 聚焦后画一帧：光标会在这一帧登记第一个唤醒点。
  double now = 10.0;
  root.set_time(now);
  root.paint_frame(canvas);
  root.clear_dirty();

  // 等到 PTY 输出彻底静下来，剩下的只有闪烁。
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // ── 连续三轮「推进到唤醒点 → 必须真画 → 必须又有了新唤醒点」 ──
  //
  // 三轮而不是一轮：唤醒链的断开表现为"某一轮之后 wake_in_ms < 0"。
  // 只验一轮的话，第一轮可能因为残留损坏区而被画到（假绿）。
  for (int round = 0; round < 3; ++round) {
    // 先把这一轮残留的帧消耗掉（PTY 静下来后应该最多一两帧）。
    for (int i = 0; i < 8 && root.dirty(); ++i) {
      root.set_time(now);
      root.paint_frame(canvas);
      root.clear_dirty();
    }
    expect(!root.dirty(), std::format("第 {} 轮：帧循环没有收敛（仍在每帧续帧）", round));

    const double wake_in_ms = root.next_wake_in_ms();
    if (!(wake_in_ms > 0.0 && wake_in_ms <= 500.0)) {
      expect(false, std::format("第 {} 轮：唤醒点缺失或异常（next_wake_in_ms={}）——光标冻结",
                                round, wake_in_ms));
      terminal->send_stop();
      return;
    }

    // 推进到唤醒点：needs_frame 必须变真（到点真拉帧）。
    now += (wake_in_ms / 1000.0) + 0.005;
    root.set_time(now);
    if (!root.dirty()) {
      expect(false, std::format("第 {} 轮：到唤醒点后 needs_frame 仍为假", round));
      terminal->send_stop();
      return;
    }

    // ★ 缺陷原形：这一帧必须**真的绘制**（否则元素无法重新登记唤醒点）。
    root.paint_frame(canvas);
    const bool painted = root.last_frame_presentable();
    root.clear_dirty();
    expect(painted, std::format("第 {} 轮：唤醒帧走了空帧路径（没绘制）——"
                                "元素无法重新登记唤醒点", round));

    // ★ 且画完之后必须有**新的**唤醒点（链闭合）。
    const double next_wake = root.next_wake_in_ms();
    expect(next_wake > 0.0 || root.dirty(),
           std::format("第 {} 轮：唤醒帧之后没有新唤醒点 —— 闪烁链断了（光标会永久冻结）",
                       round));
  }

  terminal->send_stop();
}
