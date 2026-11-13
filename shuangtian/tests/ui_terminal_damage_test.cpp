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

#include <chrono>
#include <format>
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

/// 屏幕文本里是否已经有「shell 真的在打印」的证据（出现任何非空白字符）。
///
/// ## 为何不能只看“文本非空”
///
/// shell 启动即发 `ESC[2J`（清屏）+ `ESC[H`（光标归位），屏幕模型那一刻就有
/// 一整屏空白行（`session_text(0)` = N 个 `\n`）——“非空”**当场成立**，而提示符
/// 一个字符都还没打印、shell 还没开始读输入。旧夹具把这个状态当成了就绪，
/// 接着敲的字符全落进空洞：屏幕从头到尾只有空白，最终上报整屏
///（实测 rect 高 434px = 24×18+2，正是一次清屏的量），用例报“损坏区覆盖了多行”。
///
/// ## 为何不匹配具体提示符字符串
///
/// 本文件不绑死 shell（`GEBAI_TERMINAL_SHELL` / pwsh / powershell / bash 都行），
/// 提示符不可预测（`PS D:\…> `、`user@host:~$ `…）。“有没有非空白字符”在它们
/// 之间都等价于“shell 已经开始打印了”——这正是本文件需要的前提。
[[nodiscard]] auto screen_has_ink(std::string_view text) -> bool {
  for (const char c : text) {
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') return true;
  }
  return false;
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

  /// 等 shell 真的打出**提示符**（而不是“屏幕非空”）。
  ///
  /// ## 为何不能等“屏幕文本非空”（旧写法，实测是假就绪）
  ///
  /// shell 启动即发 `ESC[2J`（清屏）+ `ESC[H`（光标归位），屏幕模型那一刻就有
  /// 24 行空白（`session_text(0)` = 24 个 `\n`）。于是“非空”**当场成立**，
  /// 而提示符一个字符都还没打印、shell 还没开始读输入——接下来敲的字符全都
  /// 落进那个空洞：屏幕从头到尾只有空白，最终上报整屏（实测 434px = 24×18+2，
  /// 正是一次清屏的量），用例报“损坏区覆盖了多行”。
  ///
  /// ## 判据用“有没有非空白字符”而不是某个具体提示符字符串
  ///
  /// 本用例不绑死 shell（`GEBAI_TERMINAL_SHELL` / pwsh / powershell 都行），
  /// 而提示符本身不可预测（pwsh `PS D:\…> `、powershell `PS …> `、
  /// POSIX `user@host:~$ `）。**“屏幕上出现了任何非空白字符”**在三种情况下
  /// 都等价于“shell 已经在打印了”——而这正是本用例需要的前提。
  [[nodiscard]] auto wait_for_prompt(int timeout_ms = 8000) -> bool {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      terminal->pump();
      draw();
      if (screen_has_ink(terminal->session_text(0))) return true;
      st::platform::sleep_ms(20);
    }
    return false;
  }
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

  // 等**真提示符**出现，并把累积的整屏变化消费掉。
  //
  // ⚠ 判据必须是「屏幕上出现了提示符」，不能是「屏幕文本非空」：
  // shell 一启动就发 `ESC[2J`（清屏）+ `ESC[H`（光标归位），屏幕模型那一刻
  // 就有 24 行空白（`plain_text()` = 24 个换行）——"非空"当场成立，而提示符
  // 一个字符都还没打印。旧判据因此**立刻放行**，接着敲的字符落在了 shell
  // 还没开始读输入的窗口里：屏幕从头到尾只有那 24 行空白，最终上报整屏
  //（实测 rect 高 434px = 24×18+2，正是一次清屏的量）。
  expect(fixture.wait_for_prompt(), "shell 没在 8s 内打出提示符（环境问题，非被测缺陷）");
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
  // ⚠ **轮询等回显**，不要固定 `sleep(200)` 后只 `pump()` 一次。
  //
  // 为何（实测）：并行跑全量时机器负载高，shell 从收键到把回显字节写回 PTY
  // 可能超过 200ms——那一次 `pump()` 抽到空，`pending()` 自然无效，用例就
  // 报“单行输出后没有任何损坏上报”。它**不是缺陷**，是夹具等得不够久；
  // 而固定等待无论改多少都是碰运气（负载一变又红）。
  //
  // 轮询的边界仍严：**只等“有没有上报”**，一旦上报就立刻拿它去断言几何；
  // 超时（3s）才判失败——真缺陷（上报整块/不上报）依然会被捉住。
  Terminal::DamageReport report{};
  for (int i = 0; i < 150; ++i) {   // 150 × 20ms = 3s 上限
    st::platform::sleep_ms(20);
    fixture.terminal->pump();
    // ⚠ **先取损坏区、再 draw**：`draw()` 里的 `paint_frame` 帧首会消费掉它
    // （本文件 `Fixture::pending` 的注释写着这一点），顺序反了就恒取到空。
    report = fixture.pending();
    if (report.valid) break;
    fixture.draw();
  }

  expect(report.valid, "单行输出后 3s 内没有任何损坏上报 —— 画面会停在旧内容上");
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

// ─────────────────────────────────────────────────────────────────────────────
// 夹具就绪判据（“屏幕非空”≠“shell 已就绪”）
// ─────────────────────────────────────────────────────────────────────────────
//
// 本文件上一条用例曾经用 `session_text(0)` **非空**当“提示符到了”，而 shell 启动
// 即发 `ESC[2J`：那一刻屏幕就是一整屏空白（N 个 `\n`），于是“非空”当场成立，
// 接着敲的字符全落进 shell 还没开始读输入的窗口——屏幕从头到尾只有空白，
// 最终上报整屏 434px（24×18+2），用例报“损坏区覆盖了多行”。
//
// 下面直接钉住那个判据本身（不需要真 shell，因此结果是确定的）：
ST_TEST(terminal_damage_readiness_requires_ink_not_just_nonempty) {
  using st::test::Registry;
  // ① 清屏后的真实形态：一整屏空白行 —— 必须判为**未就绪**。
  ST_CHECK(!screen_has_ink(std::string(24, '\n')));
  ST_CHECK(!screen_has_ink("\n\n\n"));
  ST_CHECK(!screen_has_ink(""));
  ST_CHECK(!screen_has_ink("   \t\r\n "));
  // ② 真打印了才就绪（三种 shell 的提示符形态）。
  ST_CHECK(screen_has_ink("PS D:\\code> "));
  ST_CHECK(screen_has_ink("user@host:~$ "));
  ST_CHECK(screen_has_ink("st-test# "));
  (void)Registry::instance();
}
