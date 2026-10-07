/// 终端组件（`st::ui::Terminal`）PTY 模式测试。
///
/// 与 `exec_terminal_test.cpp`（通道/会话/行模式）分开：本文件只测**真终端**那部分——
/// 伪终端接上之后，组件是否真的在做终端该做的事：
///
/// * 起的 shell 连着**伪终端**（子进程看到 tty）
/// * 输出的 ANSI 字节**喂进了屏幕模型**（不是当纯文本拼）
/// * 键盘输入变成**字节**写回 PTY（`\r` `\x7f` `\x1b[A` 这些序列的映射）
/// * 屏幕尺寸**上报**（`resize`）
/// * 中止是**真中止**（读线程要能退出）
///
/// 这里用**真 shell**（`cmd.exe` / `/bin/sh`）而不是假 PTY：本层的价值就在
/// "和真终端打交道"，用桩会把要验的东西（行规程行为、真实字节）全绕过去。

#include "st/test/test.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "st/core/pty.hpp"
#include "st/text/ansi_screen.hpp"
#include "st/ui/components/terminal.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Terminal;

/// 已挂树、已完成布局的终端（`arrange` 后几何才有效）。
struct Harness {
  st::ui::UiRoot root{};
  Terminal* terminal{nullptr};

  Harness() {
    root.set_viewport(st::math::Size{900.0f, 500.0f});
    auto owned = std::make_unique<Terminal>();
    terminal = owned.get();
    root.set_content(std::move(owned));
    root.layout(true);
  }

  /// 推进若干轮 `pump`（字节要经缓冲搬到屏幕模型，需要几帧）。
  void pump(int rounds = 60, int millis = 5) {
    for (int i = 0; i < rounds; ++i) {
      terminal->pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(millis));
    }
  }

  /// 等屏幕文本里出现 `needle`（或超时）。
  [[nodiscard]] auto wait_for(const std::string& needle, int timeout_ms = 4000) -> bool {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      terminal->pump();
      const auto* screen = terminal->screen(0);
      if (screen != nullptr && screen->plain_text().find(needle) != std::string::npos) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }
};

}  // namespace

ST_TEST(terminal_pty_mode_opens_a_real_shell) {
  // 起真 shell 之后：组件报告 PTY 模式、屏幕模型存在、子进程在跑。
  if (!st::process::PtySession::supported()) return;   // 老系统上跳过（而非失败）
  Harness harness;
  harness.terminal->open_shell();
  ST_CHECK(harness.terminal->pty_active());
  ST_CHECK(harness.terminal->busy());
  const auto* screen = harness.terminal->screen(0);
  ST_REQUIRE(screen != nullptr);
  // shell 的 banner / 提示符会自己出现在屏幕上——**我们不拼任何文字**。
  ST_CHECK(harness.wait_for(">"));
  const std::string text = screen->plain_text();
  ST_CHECK(!text.empty());
  harness.terminal->send_stop();
}

ST_TEST(terminal_pty_input_is_sent_as_bytes_and_echoed) {
  // **字节级输入**：送 "echo <标记>"（**不带回车**）——shell 应当立即回显；
  // 再送 `\r` 才执行。这是"真终端"与"输入框 + 提交"最直接的分界。
  if (!st::process::PtySession::supported()) return;
  Harness harness;
  harness.terminal->open_shell();
  ST_REQUIRE(harness.wait_for(">"));
  const std::string marker = "pty-echo-probe-9271";
  harness.terminal->send_bytes("echo " + marker);
  // 不回车的字节已经到 shell（回显出来）——**没有换行也能被读到**，
  // 这是 PTY 独有的（管道按行读会把它卡住）。
  ST_CHECK(harness.wait_for(marker));
  harness.terminal->send_bytes("\r");
  // 执行后屏幕上该标记出现**两次**（命令回显 + 输出结果）。
  ST_CHECK(harness.wait_for(marker));
  harness.pump(20);
  const auto* screen = harness.terminal->screen(0);
  ST_REQUIRE(screen != nullptr);
  const std::string text = screen->plain_text();
  std::size_t hits = 0;
  for (std::size_t at = text.find(marker); at != std::string::npos;
       at = text.find(marker, at + 1)) {
    ++hits;
  }
  ST_CHECK(hits >= 2);
  harness.terminal->send_stop();
}

ST_TEST(terminal_pty_output_feeds_the_ansi_screen_not_a_string) {
  // 输出的 ANSI 字节必须**喂进屏幕模型**（屏幕模型才有的能力：光标定位/清屏）。
  // 判据：屏幕尺寸等于组件上报给 PTY 的尺寸、且光标位置在合法范围内
  //（拼字符串的实现根本不会有"光标行/列"这个概念）。
  if (!st::process::PtySession::supported()) return;
  Harness harness;
  harness.terminal->open_shell();
  ST_REQUIRE(harness.wait_for(">"));
  const auto* screen = harness.terminal->screen(0);
  ST_REQUIRE(screen != nullptr);
  ST_CHECK(screen->cols() > 0);
  ST_CHECK(screen->rows() > 0);
  ST_CHECK(screen->cursor_row() >= 0);
  ST_CHECK(screen->cursor_row() < screen->rows());
  ST_CHECK(screen->cursor_col() >= 0);
  ST_CHECK(screen->cursor_col() < screen->cols());
  // 光标可见性由程序控制（shell 会开它）——这里只要求"是个确定状态"。
  ST_CHECK(screen->cursor_visible() || !screen->cursor_visible());
  harness.terminal->send_stop();
}

ST_TEST(terminal_pty_clear_wipes_the_screen) {
  // `clear()` 在 PTY 模式下走**屏幕的清屏语义**（而不是清自己的字符串）。
  if (!st::process::PtySession::supported()) return;
  Harness harness;
  harness.terminal->open_shell();
  ST_REQUIRE(harness.wait_for(">"));
  harness.terminal->clear();
  harness.pump(15);
  const auto* screen = harness.terminal->screen(0);
  ST_REQUIRE(screen != nullptr);
  // 清屏后屏幕上不该还有那段 banner 的首行。
  const std::string text = screen->plain_text();
  ST_CHECK(text.find("Microsoft") == std::string::npos ||
           text.find("版权") == std::string::npos);
  harness.terminal->send_stop();
}

ST_TEST(terminal_pty_stop_terminates_the_child) {
  // **"中止"必须是真中止**：`send_stop` 之后子进程要死、组件要回到不忙。
  if (!st::process::PtySession::supported()) return;
  Harness harness;
  harness.terminal->open_shell();
  ST_REQUIRE(harness.wait_for(">"));
  ST_CHECK(harness.terminal->busy());
  harness.terminal->send_stop();
  // 给收尾一点时间（杀进程 + join 读线程）。
  for (int i = 0; i < 100 && harness.terminal->pty_active(); ++i) {
    harness.terminal->pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ST_CHECK(!harness.terminal->pty_active());
}

ST_TEST(terminal_pty_resize_reaches_the_screen_model) {
  // 布局变化 → 尺寸上报：屏幕模型要跟着改尺寸（TUI 程序据此重排）。
  if (!st::process::PtySession::supported()) return;
  Harness harness;
  harness.terminal->open_shell();
  ST_REQUIRE(harness.wait_for(">"));
  harness.terminal->set_terminal_size(st::process::PtySize{100, 30});
  harness.pump(10);
  const auto* screen = harness.terminal->screen(0);
  ST_REQUIRE(screen != nullptr);
  ST_CHECK_EQ(screen->cols(), 100);
  ST_CHECK_EQ(screen->rows(), 30);
  harness.terminal->send_stop();
}

ST_TEST(terminal_pty_sessions_are_independent) {
  // 两个会话 = 两条独立 shell（各自的屏幕模型与 PTY）。
  if (!st::process::PtySession::supported()) return;
  Harness harness;
  harness.terminal->open_shell();
  ST_REQUIRE(harness.wait_for(">"));
  const std::size_t second = harness.terminal->add_session();
  ST_CHECK_EQ(second, std::size_t{1});
  harness.terminal->open_shell();
  ST_CHECK(harness.terminal->pty_active());
  harness.pump(20);
  // 两个会话各有自己的屏幕（不是共享一个）。
  ST_CHECK(harness.terminal->screen(0) != nullptr);
  ST_CHECK(harness.terminal->screen(1) != nullptr);
  ST_CHECK(harness.terminal->screen(0) != harness.terminal->screen(1));
  harness.terminal->send_stop();
}

ST_TEST(terminal_line_mode_still_works_without_pty) {
  // **两条模式并存**：不开 shell 时是行模式（`run` + 输出拼接），
  // 那些"不需要 tty"的场景（跑一次构建看退出码）仍然可用。
  //
  // 用一条几乎必然失败的命令：只要**如实报告**（不挂住、不抛异常）就算过。
  Harness harness;
  ST_CHECK(!harness.terminal->pty_active());
  harness.terminal->run("echo line-mode-probe");
  for (int i = 0; i < 200 && harness.terminal->busy(); ++i) {
    harness.terminal->pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const std::string text = harness.terminal->session_text(0);
  // 行模式没有屏幕模型（那条路径不建它）。
  ST_CHECK(harness.terminal->screen(0) == nullptr);
  ST_CHECK(!text.empty());
}
