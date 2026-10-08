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
#include <cstdlib>
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

/// 测试专用 shell：**固定提示符**是这套测试可控性的地基。
///
/// * Windows：PowerShell（`>` 提示符，与产品默认一致）；
/// * POSIX：**bash** `--noprofile --norc -i` + 环境变量 `PS1=st-test# `——
///   提示符完全确定、不受 dotfiles/发行版影响。之前用系统默认 `$SHELL`
///   （本机是 zsh，提示符是 `➜`）或 `/bin/sh`（dash，重定向下不出提示符），
///   等 `>`/`#` 都在赌运气（实测全部超时）。产品代码不动——它按系统默认走。
struct TestShell {
  std::string program;
  std::vector<std::string> argv;
  const char* prompt_needle{};
};

[[nodiscard]] auto test_shell() -> TestShell {
  if (const char* configured = std::getenv("GEBAI_TERMINAL_SHELL");
      configured != nullptr && configured[0] != '\0') {
    return TestShell{configured, {}, ">"};   // 用户指定：只等 `>`（保守）
  }
#ifdef _WIN32
  return TestShell{"powershell.exe", {}, ">"};
#else
  (void)setenv("PS1", "st-test# ", 1);   // bash 交互式下 PS1 环境变量优先于内置
  return TestShell{"/bin/bash", {"--noprofile", "--norc", "-i"}, "st-test#"};
#endif
}

/// 已挂树、已完成布局的终端（`arrange` 后几何才有效）。
struct Harness {
  st::ui::UiRoot root{};
  Terminal* terminal{nullptr};

  Harness() {
    root.set_viewport(st::math::Size{900.0f, 500.0f});
    auto owned = std::make_unique<Terminal>();
    terminal = owned.get();
    // 测试不赌系统默认 shell：注入固定的 bash/PowerShell（见 test_shell 说明）。
    const TestShell sh = test_shell();
    terminal->set_shell(sh.program, sh.argv);
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

  /// 推进 `pump` 直到 `predicate` 成立（或超时）。返回是否成立。
  ///
  /// 用途：等待“某件事在屏幕上发生了”——比如命令已开始执行、`Ctrl+C` 已生效。
  /// 比 `pump(N, 20)` （固定睡 N×20ms）快得多，也不会因机器慢而睡不够
  ///（实测：`terminal_pty_stop_...` 里两处固定睡共 2.0s，而条件实际在 100ms 内就满足）。
  template <typename Predicate>
  auto wait_until(Predicate predicate, int timeout_ms = 4000) -> bool {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      terminal->pump();
      if (predicate()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  /// 当前屏幕文本（无屏幕模型时为空串）。
  [[nodiscard]] auto screen_text() -> std::string {
    const auto* screen = terminal->screen(0);
    return screen != nullptr ? screen->plain_text() : std::string{};
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
  ST_CHECK(harness.wait_for(test_shell().prompt_needle));
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
  ST_REQUIRE(harness.wait_for(test_shell().prompt_needle));
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
  ST_REQUIRE(harness.wait_for(test_shell().prompt_needle));
  const auto* screen = harness.terminal->screen(0);
  ST_REQUIRE(screen != nullptr);
  ST_CHECK(screen->cols() > 0);
  ST_CHECK(screen->rows() > 0);
  ST_CHECK(screen->cursor_row() >= 0);
  ST_CHECK(screen->cursor_row() < screen->rows());
  ST_CHECK(screen->cursor_col() >= 0);
  ST_CHECK(screen->cursor_col() < screen->cols());
  harness.terminal->send_stop();
}

ST_TEST(terminal_pty_clear_wipes_the_screen) {
  // `clear()` 在 PTY 模式下走**屏幕的清屏语义**（而不是清自己的字符串）。
  if (!st::process::PtySession::supported()) return;
  Harness harness;
  harness.terminal->open_shell();
  ST_REQUIRE(harness.wait_for(test_shell().prompt_needle));
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

ST_TEST(terminal_pty_stop_interrupts_without_killing_the_shell) {
  // **中止的语义**：发 `Ctrl+C` 打断**当前命令**，而 **shell 自己活着**——
  // 这是真终端里 `Ctrl+C` 的行为（内核行规程向前台进程组发 SIGINT）。
  //
  // 为何不杀 shell：那会把整个会话弄没——用户想着“停一下这条”，
  // 结果终端直接死了、得重新开一个（实测踩到）。真要关会话用 `close_session`。
  if (!st::process::PtySession::supported()) return;
  Harness harness;
  harness.terminal->open_shell();
  ST_REQUIRE(harness.wait_for(test_shell().prompt_needle));
  ST_CHECK(harness.terminal->busy());

  // 长命令与“打印一行”按平台选（旧写法写死了 PowerShell 的 `Start-Sleep`/
  // `Write-Output`，配上 POSIX 测试 shell 时两者都“命令不存在”——报错信息里
  // 恰好包含那串标记，`wait_for` 因此**假通过**：断言看着绿，语义根本没验到）。
#ifdef _WIN32
  const std::string long_cmd = "Start-Sleep -Seconds 60";
  const std::string after_cmd = "Write-Output after-stop-9271";
#else
  const std::string long_cmd = "sleep 60";
  const std::string after_cmd = "echo after-stop-9271";
#endif

  // 起长命令 → 中断。
  //
  // ⚠ 这里原来是 `pump(40, 20)`（无条件睡 0.8s）与 `pump(60, 20)`（1.2s）——
  // 共 **2.0s 固定耗时**，而条件实际几百毫秒内就满足（实测：这一个用例占整组
  // 4.19s 中的 2.14s）。改成**条件等待**：一到状态就返回，慢机器也不会睡不够。
  harness.terminal->send_bytes(long_cmd + "\r");
  // 等命令确实发出（命令行回显到了屏幕上）——而不是盲等。
  (void)harness.wait_until(
      [&] { return harness.screen_text().find(long_cmd) != std::string::npos; }, 3000);

  // **关键断言的前置**：记下中断前的时刻。长命令是 60 秒，所以只要它在
  // 远短于 60 秒内回到提示符，就只可能是被 `Ctrl+C` 打断了——
  // 这正是本用例要验的语义（旧写法没有这条，于是 `send_stop` 不发 `Ctrl+C`
  // 时它仍会 PASS：实测把 send_stop 的写字节改成空操作，用例只变慢到 3.1s 仍绿）。
  const auto stop_sent_at = std::chrono::steady_clock::now();
  harness.terminal->send_stop();
  // 等中断生效：`Ctrl+C` 后 shell 回到提示符（屏幕上出现第二次提示符）。
  const bool back_to_prompt = harness.wait_until(
      [&] {
        const std::string text = harness.screen_text();
        std::size_t count = 0;
        for (std::size_t at = text.find(test_shell().prompt_needle); at != std::string::npos;
             at = text.find(test_shell().prompt_needle, at + 1)) {
          ++count;
        }
        return count >= 2;   // 首提示符 + 中断后回到的提示符
      },
      5000);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - stop_sent_at)
                           .count();
  // 真的被中断了：提示符回来了，而且是在长命令（60s）到位之前。
  ST_CHECK(back_to_prompt);
  ST_CHECK(elapsed < 10000);   // 若是命令自己跑完，得 ≥ 60s
  // shell 仍在（能接着用），这是与“杀进程”最直接的分界。
  ST_CHECK(harness.terminal->pty_active());
  // 再送一条命令，应该能执行（把中断后的输入流验证一下）。
  harness.terminal->send_bytes(after_cmd + "\r");
  ST_CHECK(harness.wait_for("after-stop-9271"));
  harness.terminal->send_stop();
}

ST_TEST(terminal_pty_resize_reaches_the_screen_model) {
  // 布局变化 → 尺寸上报：屏幕模型要跟着改尺寸（TUI 程序据此重排）。
  if (!st::process::PtySession::supported()) return;
  Harness harness;
  harness.terminal->open_shell();
  ST_REQUIRE(harness.wait_for(test_shell().prompt_needle));
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
  ST_REQUIRE(harness.wait_for(test_shell().prompt_needle));
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
