/// 终端**光标闪烁续帧**回归测试（缺陷：闪烁其实完全不闪）。
///
/// ### 本轮钉住的缺陷（2026-10-08，用户报「光标闪烁不稳定」）
///
/// `Terminal::paint_content` 用 `fmod(context.time_seconds, 1.0)` 决定光标显隐，
/// 却**从不调 `request_animation()`**——而"还要不要下一帧"由它汇总：
/// `UiRoot` 帧末 `collect_animation_requests()` → `followup_`，
/// 下一帧起 `UiRoot::dirty()`（`needs_frame()`）才为真。
///
/// 于是重绘平静下来后不再有新帧，光标**冻在它碰巧停住的相位上**
/// （实测：静置 6 秒、30 次采样，像素**零变化**）。
/// 用户说"不稳定"是准确的——他们看到的是"有时亮着一直不灭"。
///
/// ### 判据
///
/// **画过一帧、且没有新输入时，`UiRoot::dirty()` 必须仍为真**
/// （=「还有动画要下一帧」）。这条与"光标画在哪儿"无关，
/// 直接对应"闪烁能不能继续"这个机制本身。
///
/// 用 `focused()` 前置：**失焦的终端不该占用帧预算**（那是纯浪费），
/// 因此本用例必须先给终端焦点——这同时钉住了这个刻意的行为。

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
  [[nodiscard]] auto ellipsize(std::string_view utf8, float, float) const
      -> std::string override {
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

/// 带详情的断言（`ST_CHECK` 的宏消息只有源码文本，说不出为啥失败）。
void expect(bool ok, std::string message) {
  ::st::test::Registry::instance().count_check();
  if (!ok) ::st::test::Registry::instance().record_failure(__FILE__, __LINE__, std::move(message));
}

}  // namespace

ST_TEST(terminal_cursor_requests_followup_frames_while_focused) {
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
    root.paint_frame(canvas);
    root.clear_dirty();
  }
  expect(port.draws > 0, "终端一帧都没画（夹具没跑起来）");
  expect(root.set_focus(terminal), "无法把焦点给终端");
  expect(terminal->focused(), "焦点没落在终端上");

  // 让光标在**已聚焦**状态下画一帧（此时它才会请求续帧）。
  root.paint_frame(canvas);
  root.clear_dirty();

  // **核心判据：帧循环必须不收敛**。
  //
  // 为什么不看 `terminal->animation_requested()`：那个开关在**绘制时**就被
  // `collect_animation_requests` 消费掉了，画完后读它永远是 false
  //——第一版用例就这么写过，得到的是假结论。
  // 也不看单帧的 `root.dirty()`：PTY 输出产生的损坏区会把它与动画混为一谈。
  //
  // 用**循环能不能停下来**最直接：光标每秒闪两次，帧就永远有下一帧。
  // （须成对调 `paint_frame` + `clear_dirty`——后者才是把本帧的动画请求
  //   滚成 `followup_` 的地方，与应用的帧循环同形。）
  int frames = 0;
  while (root.dirty() && frames < 40) {
    root.paint_frame(canvas);
    root.clear_dirty();
    ++frames;
  }
  expect(frames >= 40, std::format("帧循环在 {} 帧后收敛了 —— 光标不会继续闪（缺陷原形）",
                                   frames));

  terminal->send_stop();
}

ST_TEST(terminal_cursor_does_not_request_frames_when_unfocused) {
  // 反向护栏：**失焦**时不该因为光标而续帧（省帧预算）。
  // 与上一条同一判据、相反结论：失焦时帧循环应当**收敛**。
  // 若实现把 `request_animation` 写成无条件，本用例变红。
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
    root.paint_frame(canvas);
    root.clear_dirty();
  }
  expect(port.draws > 0, "终端一帧都没画（夹具没跑起来）");

  // 不调 pump：让 PTY 输出静下来，剩下的“还要不要帧”就只剩光标。
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  expect(!terminal->focused(), "夹具前提不成立：终端不该有焦点");

  int frames = 0;
  while (root.dirty() && frames < 40) {
    root.paint_frame(canvas);
    root.clear_dirty();
    ++frames;
  }
  expect(frames < 10, std::format("失焦时帧循环跑了 {} 帧才停 —— 白烧帧预算", frames));

  terminal->send_stop();
}
