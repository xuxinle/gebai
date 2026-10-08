// 量清 `terminal_pty_stop_interrupts_without_killing_the_shell` 为何偶发红。
//
// 该用例实测 3 轮复跑：红 2 轮（卡满 9.5 s）、绿 1 轮（637 ms）。两个失败断言：
//   · back_to_prompt（看到第 2 个提示符）
//   · wait_for("after-stop-9271")
//
// 本探针**只报状态、不下结论**：把「发 Ctrl+C 的那一刻」与「之后的演化」逐轮打出来，
// 看是真没被中断、还是被中断了但屏幕没收到/没渲染。
//
// 用法：pwsh -File tools/build_probes.ps1 -Only pty_stop_probe   （需先 st build）
//       build/probe/pty_stop_probe.exe [重复轮数]

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "st/core/print.hpp"
#include "st/core/pty.hpp"
#include "st/text/ansi_screen.hpp"
#include "st/ui/components/terminal.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Terminal;
using Clock = std::chrono::steady_clock;

const char* prompt_needle() {
#ifdef _WIN32
  return ">";
#else
  return "st-test#";
#endif
}

std::string long_cmd() {
  // 先打一个“已开始执行”的标记，再睡——**这是可观测的前置条件**。
  //
  // 为什么不单靠“命令行回显”：回显发生在**输入时**（shell 收到行、回显给了 PTY），
  // 而命令**开始执行**是之后的事。两者之间有一个真实窗口，竞态就住在里面：
  // 实测直接发 Ctrl+C 时 12 轮红 6 轮（红的那几轮屏幕**一个字节都不再增长**）。
#ifdef _WIN32
  return "Write-Output ('ru'+'nning-9271'); Start-Sleep -Seconds 60";
#else
  return "echo 'ru''nning-9271'; sleep 60";
#endif
}

/// 修复前的命令形态（无标记、直接睡）——用来**复现**原来的竞态。
std::string bare_long_cmd() {
#ifdef _WIN32
  return "Start-Sleep -Seconds 60";
#else
  return "sleep 60";
#endif
}

std::string after_cmd() {
#ifdef _WIN32
  return "Write-Output after-stop-9271";
#else
  return "echo after-stop-9271";
#endif
}

/// 长命令的“已开始执行”标记（见 `long_cmd`）。
constexpr const char* kRunningMarker = "running-9271";

int count_needle(const std::string& text, const std::string& needle) {
  int count = 0;
  for (std::size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + 1)) {
    ++count;
  }
  return count;
}

struct Harness {
  st::ui::UiRoot root{};
  Terminal* terminal{nullptr};

  Harness() {
    root.set_viewport(st::math::Size{900.0f, 500.0f});
    auto owned = std::make_unique<Terminal>();
    terminal = owned.get();
#ifdef _WIN32
    terminal->set_shell("powershell.exe", {});
#else
    (void)setenv("PS1", "st-test# ", 1);
    terminal->set_shell("/bin/bash", {"--noprofile", "--norc", "-i"});
#endif
    root.set_content(std::move(owned));
    root.layout(true);
  }

  auto screen_text() -> std::string {
    const auto* screen = terminal->screen(0);
    return screen != nullptr ? screen->plain_text() : std::string{};
  }

  template <typename Predicate>
  auto wait_until(Predicate predicate, int timeout_ms) -> bool {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
      terminal->pump();
      if (predicate()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }
};

void one_round(int round, int settle_ms, bool early_stop) {
  st::print("\n════ 轮次 {} ════\n", round);
  Harness harness;
  harness.terminal->open_shell();
  if (!harness.wait_until([&] {
        return harness.screen_text().find(prompt_needle()) != std::string::npos;
      }, 4000)) {
    st::print("  首个提示符都没等到 —— 环境问题，本轮跳过\n");
    harness.terminal->send_stop();
    return;
  }
  st::print("  首提示符 OK · busy={} pty_active={}\n", harness.terminal->busy() ? 1 : 0,
            harness.terminal->pty_active() ? 1 : 0);

  const std::string cmd = early_stop ? bare_long_cmd() : long_cmd();
  harness.terminal->send_bytes(cmd + "\r");
  const bool echo_seen = harness.wait_until(
      [&] { return harness.screen_text().find(cmd) != std::string::npos; }, 3000);
  // **前置条件：命令真的开始执行了**（标记是 shell 执行到那一行才打出来的）。
  // 只等回显不够——回显是“收到了输入”，不等于“开始执行了”。
  //
  // `early_stop` 下**故意不等**：那是复现原来的竞态（见文件头）。
  const bool running_seen = early_stop
      ? false
      : harness.wait_until(
            [&] { return harness.screen_text().find(kRunningMarker) != std::string::npos; }, 3000);
  const int prompts_before = count_needle(harness.screen_text(), prompt_needle());
  st::print("  回显={} · 已开始执行={} · 发停止前提示符数={}\n", echo_seen ? "有" : "无",
            early_stop ? "（不等前置）" : (running_seen ? "是" : "**否**"), prompts_before);

  // settle_ms 旋钮只为**对照实验**用（验证“就是启动竞态”而非别的）。
  if (settle_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));

  const auto t_stop = Clock::now();
  const std::string before_stop = harness.screen_text();
  harness.terminal->send_stop();

  // 发完停止立刻看一次：区分“根本没回显”与“回显了但没回到提示符”
  harness.terminal->pump();
  const std::string right_after = harness.screen_text();
  st::print("  发停止前 {} 字符 → 后 {} 字符（屏幕{}）\n", before_stop.size(),
            right_after.size(),
            right_after.size() < before_stop.size() ? "**缩小了**" : "未缩小");

  const bool back = harness.wait_until(
      [&] { return count_needle(harness.screen_text(), prompt_needle()) >= 2; }, 5000);
  const auto ms_back = std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - t_stop)
                           .count();
  st::print("  back_to_prompt={}（{} ms）· 提示符数={} · pty_active={}\n",
            back ? "是" : "**否**", ms_back,
            count_needle(harness.screen_text(), prompt_needle()),
            harness.terminal->pty_active() ? 1 : 0);

  harness.terminal->send_bytes(after_cmd() + "\r");
  const bool after = harness.wait_until(
      [&] { return harness.screen_text().find("after-stop-9271") != std::string::npos; }, 4000);
  st::print("  after-stop 可见={}\n", after ? "是" : "**否**");

  // 失败现场：关键区分——再发一次 Ctrl+C，看会话是“吞了这一次中断”还是“彻底僵住”。
  // 前者是测试的时序问题，后者是产品的 bug（会话不可用）。
  if (!back) {
    const auto t2 = Clock::now();
    harness.terminal->send_stop();
    const bool recovered = harness.wait_until(
        [&] { return count_needle(harness.screen_text(), prompt_needle()) >= 2; }, 3000);
    const auto ms2 =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t2).count();
    st::print("  再发一次 Ctrl+C → 恢复={}（{} ms）· 文本长度={}\n",
              recovered ? "是（第一次被吞了）" : "**否（会话僵住）**", ms2,
              harness.screen_text().size());
  }

  if (!back || !after) {
    const auto* screen = harness.terminal->screen(0);
    const std::string text = harness.screen_text();
    st::print("  ── 失败现场 ──\n");
    st::print("  screen(0) 空指针={} · 文本长度={} · 会话数={}\n",
              screen == nullptr ? "是" : "否", text.size(),
              harness.terminal->session_count());
    st::print("  |{}|\n", text.substr(0, 300));
  }
  harness.terminal->send_stop();
}

}  // namespace

int main(int argc, char** argv) {
  const int rounds = argc > 1 ? std::atoi(argv[1]) : 5;
  const int settle_ms = argc > 2 ? std::atoi(argv[2]) : 0;
  const bool early_stop = argc > 3 && std::atoi(argv[3]) != 0;
  if (!st::process::PtySession::supported()) {
    st::print("本环境不支持 PTY：探针不适用\n");
    return 0;
  }
  st::print("轮数={} · 发 Ctrl+C 前的额外延时={} ms · 不等前置={}\n", rounds, settle_ms,
            early_stop ? 1 : 0);
  for (int i = 1; i <= rounds; ++i) one_round(i, settle_ms, early_stop);
  return 0;
}
