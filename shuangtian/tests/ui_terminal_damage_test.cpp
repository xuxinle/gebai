// 终端**按行上报损坏**的回归测试（缺陷史：一次按键回显触发整帧重绘）。
//
// ### 现象
//
// 在 gbcode 底部终端里敲一个字，整块界面重画一遍：实测
// `partial_frame=false`、`dirty_rect` = 全窗口、`paint≈28.7ms`、`painted_elements=51`；
// 而同一时刻改侧栏文字只需局部重绘（`paint≈15ms / 41 元素`）。
//
// ### 根因
//
// `Terminal::pump()` 喂完 PTY 字节后只调了一次 `mark_dirty()`——那上报的是
// **整个组件 bounds**（底部面板占视口 30%），与其它元素的损坏区合并后恒超
// 「损坏区超 55% 就用整帧更划算」的阈值，增量重绘形同虚设。而屏幕模型其实
// **早就**维护了逐行脏标记（`AnsiScreen::set_dirty`/`take_dirty_rows`），只是没人调。
//
// ### 判据
//
// ① 一次单行输出上报的损坏区必须是**一条行带**（高≈一行），不是整块；
// ② 位置必须落在终端内（不能乱报）；
// ③ 反向：还没画过（几何未知）时要能兜底整块——`needs_full` 与“整块”都不许
//    变成“什么都不报”（那会残留旧像素）。

#include "st/test/test.hpp"

#include <memory>
#include <string>

#include "st/core/log.hpp"
#include "st/core/pty.hpp"
#include "st/core/wait.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/ansi_screen.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/terminal.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Terminal;

/// 等宽假端口：每格 8px 宽、18px 高（绘制路径用它量格宽/行高）。
class CellPort final : public st::ui::TextPort {
 public:
  static constexpr float kCell{8.0f};
  static constexpr float kLine{18.0f};
  [[nodiscard]] auto measure(std::string_view utf8, float) const -> st::math::Size override {
    return st::math::Size{kCell * static_cast<float>(utf8.size()), kLine};
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float,
                                   st::text::FontRole) const -> float override {
    return kCell * static_cast<float>(utf8.size());
  }
  [[nodiscard]] auto line_height(float) const -> float override { return kLine; }
  [[nodiscard]] auto ascent(float) const -> float override { return kLine * 0.75f; }
  [[nodiscard]] auto descent(float) const -> float override { return kLine * 0.25f; }
  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float,
            st::math::Color color, st::text::FontRole, float, bool) const override {
    canvas.fill_rect(st::math::Rect{origin.x, origin.y, kCell * static_cast<float>(utf8.size()),
                                    kLine * 0.7f},
                     st::raster::Paint::solid(color));
  }
  [[nodiscard]] auto ellipsize(std::string_view text, float, float) const
      -> std::string override {
    return std::string{text};
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float, float) const
      -> std::vector<std::string_view> override {
    return {utf8};
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float, float, std::size_t) const
      -> std::vector<std::string> override {
    return {std::string{utf8}};
  }
};

void expect(bool ok, std::string message) {
  ::st::test::Registry::instance().count_check();
  if (!ok) ::st::test::Registry::instance().record_failure(__FILE__, __LINE__, std::move(message));
}

/// 夹具：真 PTY（`open_shell`）+ 已画过一帧（几何/格宽就位）。
///
/// 用真 shell 而不是桩：这条契约的输入就是「真终端吐出的字节让屏幕模型标脏了哪几行」，
/// 桩会把要验的那一段（`AnsiScreen::set_dirty` 的行号）整个绕过去。
struct Fixture {
  CellPort port{};
  st::ui::UiRoot root{};
  Terminal* terminal{nullptr};
  std::unique_ptr<st::raster::Canvas> canvas{};

  Fixture() {
    root.set_theme(st::ui::Theme{st::ui::Theme::dark()});
    root.set_viewport(st::math::Size{900.0f, 600.0f});
    root.set_text_port(&port);
    auto host = std::make_unique<st::ui::Panel>();
    host->set_id("host");
    host->style().grow = true;
    auto owned = std::make_unique<Terminal>();
    terminal = owned.get();
    terminal->set_id("term-damage");
    terminal->style().grow = true;
    host->add_child(std::move(owned));
    root.set_content(std::move(host));
    root.layout(true);
    canvas = std::make_unique<st::raster::Canvas>(900, 600, 1.0f);
  }

  void draw() {
    root.set_time(1.0);
    root.paint_frame(*canvas);
    root.clear_dirty();
  }

  /// 取走内容根基上累积的损坏区（`paint_frame` 帧首会消费它，所以要单独取）。
  [[nodiscard]] auto pending() -> Terminal::DamageReport { return root.content()->take_damage(); }
};

}  // namespace

ST_TEST(terminal_single_line_output_reports_row_band_not_whole_bounds) {
  if (!st::process::PtySession::supported()) return;
  Fixture fixture;
  // **先把 PTY 尺寸定死**，让它和布局算出来的一致（本例是 900x600 视口、
  // 8x18 格）：不这样做，首帧会 resize 一次，而 PowerShell 在 resize 后会
  // **重画整屏**——那是真变化，会把本用例要看的“单行”淹掉。
  fixture.terminal->set_terminal_size(st::process::PtySize{111, 31});
  fixture.terminal->open_shell();
  expect(fixture.terminal->pty_active(), "PTY 没起来（本用例的前提）");

  const st::math::Rect bounds = fixture.terminal->bounds();
  expect(!bounds.is_empty(), "终端几何为空（夹具没排上布局）");

  // 让提示符先出来，并把累积的整屏变化消费掉。
  for (int i = 0; i < 80; ++i) {
    fixture.terminal->pump();
    fixture.draw();
    if (!fixture.terminal->session_text(0).empty()) break;
    st::platform::sleep_ms(20);
  }
  // ⚠ 先等**布局/PTY 尺寸稳定**：面板首次排布会把 PTY resize（`resize` 会
  // `mark_all_dirty`）——那是“真的整屏变”，不该混进本用例的“单行变化”里。
  // 判据：连续两次 pump 都没有上报（`pending()` 会消掉标记，所以每次都要重新累积）。
  int quiet = 0;
  for (int i = 0; i < 160 && quiet < 2; ++i) {
    fixture.terminal->pump();
    fixture.draw();
    quiet = fixture.pending().valid ? 0 : quiet + 1;
    st::platform::sleep_ms(20);
  }
  expect(quiet >= 2, "终端一直有输出，静不下来（环境太吵，本用例不适用）");
  // 尺寸已经定下来后，把前面积累的一切清干净。
  fixture.draw();
  (void)fixture.pending();

  // 打一个字符：shell 回显在同一行 ⇒ 屏幕只有那一行（加光标行）变。
  // ⚠ 不要用 `run("...")` 发命令：那会带回车 → shell 重新打印提示符 →
  // **整屏滚动**（所有行真的都变了）——那是本用例要排除的情形。
  //
  // ⚠ **先敲一个“热身”字符**：PowerShell 的 PSReadLine 在第一次接键时会
  // 初始化行编辑器并**重画整屏**（实测：首键就报出 31 行全脏）。那是真变化，
  // 会把本例要看的“单行”淹掉；热身之后才是稳态的“只重画当前行”。
  fixture.terminal->send_bytes("h");
  quiet = 0;
  for (int i = 0; i < 120 && quiet < 2; ++i) {
    st::platform::sleep_ms(20);
    fixture.terminal->pump();
    fixture.draw();
    quiet = fixture.pending().valid ? 0 : quiet + 1;
  }

  fixture.terminal->send_bytes("x");
  st::platform::sleep_ms(200);
  fixture.terminal->pump();

  const Terminal::DamageReport report = fixture.pending();
  expect(report.valid, "单行输出后没有任何损坏上报 —— 画面会停在旧内容上");
  if (!report.valid) return;
  expect(!report.needs_full, "单行输出却要求整帧重绘（needs_full=true）");
  // 终端内部的单行变化永远是**一条行带**：高≈一行、宽=输出区宽，
  // 与终端 bounds/屏数无关。
  expect(report.rect.height <= CellPort::kLine * 3.0f,         std::format("损坏区高 {:.1f}px 覆盖了多行（一行 {:.0f}px）——仍在整块上报：rect=({:.1f},{:.1f},{:.1f},{:.1f})",
                     report.rect.height, CellPort::kLine, report.rect.x, report.rect.y,
                     report.rect.width, report.rect.height));
  expect(report.rect.width <= bounds.width + 1.0f,
         std::format("损坏区宽 {:.1f}px 超出终端宽 {:.1f}px", report.rect.width, bounds.width));
  expect(report.rect.y >= bounds.y - 1.0f && report.rect.bottom() <= bounds.bottom() + 1.0f,
         std::format("损坏区纵向越出终端（y={:.1f} h={:.1f}，终端 {:.1f}..{:.1f}）", report.rect.y,
                     report.rect.height, bounds.y, bounds.bottom()));

  fixture.terminal->send_stop();
}
