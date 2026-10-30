/// 终端**光标闪烁定时唤醒**回归测试（缺陷史：闪烁烧 CPU + 闪烁冻结，两次都钉住）。
///
/// ### 第一轮（2026-10-08，用户报「光标闪烁不稳定」）
///
/// `Terminal::paint_content` 用 `fmod(context.time_seconds, 1.0)` 决定光标显隐，
/// 却**从不调 `request_animation()`**——而"还要不要下一帧"由它汇总：
/// 于是重绘平静下来后不再有新帧，光标**冻在它碰巧停住的相位上**。
///
/// ### 第二轮（2026-10-29，用户报「gbcode CPU 70%、几个示例都烧」）
///
/// 第一轮的修法把闪烁挂到 `request_animation()`（连续续帧）——修好了冻结，
/// 却引入了新缺陷：**一秒只翻转两次的光标让帧循环永不收敛**，以帧预算上限
/// （60fps）空转重绘同一张静止画面，纯 CPU 软件光栅下实测烧满 5 核。
///
/// 现在的语义（`Element::request_wake_at`）：闪烁登记**下一个相位翻转点**，
/// 帧循环把空闲等待睡到那个点——两帧之间真正睡眠。判据也随之更新：
///
/// 1. 聚焦静止时，**帧循环收敛**（`needs_frame` 为假）——不烧帧预算；
/// 2. 但 `next_wake_in_ms()` 给出**有限的正值**（闪烁仍会继续，不会冻结）；
/// 3. 把时间轴推进到唤醒点之后，`needs_frame()` 变真——到点真拉帧。
///
/// 用 `focused()` 前置：**失焦的终端不该登记唤醒**（那是纯浪费）。

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
  static constexpr float kCellWidth{8.0f};
  static constexpr float kLineHeight{16.0f};
  static constexpr float kAscent{12.0f};
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
  [[nodiscard]] auto ellipsize(std::string_view, float, float) const
      -> std::string override {
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

ST_TEST(terminal_cursor_wakes_at_flip_points_while_focused) {
  if (!st::process::PtySession::supported()) return;
  BlankTextPort port{};
  st::ui::UiRoot root{};
  root.set_theme(st::ui::Theme{st::ui::Theme::dark()});
  root.set_viewport(st::math::Size{800.0f, 400.0f});
  root.set_text_port(&port);

  auto owned = std::make_unique<Terminal>();
  Terminal* terminal = owned.get();
  terminal->set_id("term-blink");
  root.set_content(std::move(owned));
  root.layout(true);

  terminal->open_shell();
  st::raster::Canvas canvas{800, 400, 1.0f};

  // 先跑到有内容（提示符出来），并确认焦点在终端上。
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

  // 让光标在**已聚焦**状态下画一帧（此时它会登记下一个唤醒点）。
  root.set_time(10.0);
  root.paint_frame(canvas);
  root.clear_dirty();

  // ① 聚焦静止：帧循环必须收敛（不烧帧预算）。
  //    （PTY 输出静下来后剩下的只有闪烁的定时唤醒——它不该让 needs_frame 为真。）
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  int frames = 0;
  while (root.dirty() && frames < 40) {
    root.set_time(10.0 + static_cast<double>(frames) * 0.001);
    root.paint_frame(canvas);
    root.clear_dirty();
    ++frames;
  }
  expect(frames < 5, std::format("聚焦静止时帧循环跑了 {} 帧才停 —— 定时唤醒没生效，仍在连续续帧",
                                 frames));

  // ② 但必须有唤醒点（否则光标又会冻住——2026-10-08 的老缺陷回归）。
  const double wake_in_ms = root.next_wake_in_ms();
  expect(wake_in_ms > 0.0 && wake_in_ms <= 500.0,
         std::format("唤醒点缺失或异常：next_wake_in_ms={}（光标会冻结）", wake_in_ms));

  // ③ 时间推进到唤醒点之后：needs_frame 必须变真（到点真拉帧，闪烁继续）。
  root.set_time(10.0 + (wake_in_ms / 1000.0) + 0.01);
  expect(root.dirty(), "到唤醒点后 needs_frame 仍为假 —— 光标不会翻转（冻结回归）");

  terminal->send_stop();
}

ST_TEST(terminal_cursor_does_not_wake_when_unfocused) {
  // 反向护栏：**失焦**时不该登记唤醒点（省帧预算）。
  if (!st::process::PtySession::supported()) return;
  BlankTextPort port{};
  st::ui::UiRoot root{};
  root.set_theme(st::ui::Theme{st::ui::Theme::dark()});
  root.set_viewport(st::math::Size{800.0f, 400.0f});
  root.set_text_port(&port);

  auto owned = std::make_unique<Terminal>();
  Terminal* terminal = owned.get();
  terminal->set_id("term-blink-idle");
  root.set_content(std::move(owned));
  root.layout(true);

  terminal->open_shell();
  st::raster::Canvas canvas{800, 400, 1.0f};
  for (int i = 0; i < 60 && port.draws == 0; ++i) {
    terminal->pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    root.layout(false);
    root.set_time(static_cast<double>(i) * 0.03);
    root.paint_frame(canvas);
    root.clear_dirty();
  }
  expect(port.draws > 0, "终端一帧都没画（夹具没跑起来）");

  // 不调 pump：让 PTY 输出静下来，剩下的"还要不要帧"就只剩光标。
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  expect(!terminal->focused(), "夹具前提不成立：终端不该有焦点");

  root.set_time(20.0);
  root.paint_frame(canvas);
  root.clear_dirty();

  int frames = 0;
  while (root.dirty() && frames < 40) {
    root.set_time(20.0 + static_cast<double>(frames) * 0.001);
    root.paint_frame(canvas);
    root.clear_dirty();
    ++frames;
  }
  expect(frames < 10, std::format("失焦时帧循环跑了 {} 帧才停 —— 白烧帧预算", frames));
  expect(root.next_wake_in_ms() < 0.0, "失焦的终端不该登记唤醒点");

  terminal->send_stop();
}
