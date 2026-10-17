// A/B 实验：`terminal_pty_stop_interrupts_without_killing_the_shell` 偶发红的因果与修法。
//
// ## 要回答的问题
//
// 2026-10-08 的修法是「长命令先打一个命令文本里拼不出来的标记，等标记出现才发 Ctrl+C」，
// 当时验收 25 轮红 9 → 0。现在高负载下又红。为什么？
//
// ## 三种形态，只差「标记由谁打出」
//
//   P（前身：2026-10-08 之前的写法，正对照）
//       `sleep 60`，**不等标记**（看到命令行回显就发 Ctrl+C）。
//       命令回显发生在 shell 收到整行时，离命令真正接管终端还早——窗口最大。
//
//   A（现行测试写法）
//       `echo 'ru''nning-9271'; sleep 60`
//       标记由 **bash 内建 `echo`** 打出。`echo` 与 `sleep` 在 bash 眼里是**两条命令**：
//       标记出现时 bash 还**没有** fork 出 `sleep`。此后 bash 才 fork 子进程、设进程组、
//       交给终端（tcsetpgrp）、execve 成 `sleep`。在 execve 之前那个子进程跑的是
//       bash 自己的收尾代码，**SIGINT 在那里是被忽略的**——落进这段窗口的 `0x03` 就丢了。
//       所以「等标记」只把窗口**缩小**（从"整行解析前"缩到"fork→exec 之间"），没有关掉。
//
//   B（修法）
//       `/bin/sh -c "echo 'ru''nning-9271'; exec sleep 60"`
//       在 bash 眼里这是**一条**命令（`/bin/sh`）= 一个前台作业、一个进程组。
//       `/bin/sh` 打出标记后 `exec` 成 `sleep`：**同一个 pid、同一个进程组，
//       标记之后不再发生任何前台进程组交接**。窗口在结构上不存在了。
//
// ## 判据
//
// 三臂**交替**跑（同负载条件下逐轮轮转，避免"先跑的那臂赶上别的构建"这类偏置），
// 每臂各 N 轮。期望：P 必有红（灵敏度正对照——它证明本探针确实能检出这个窗口），
// A 在负载下红，B 零红。P 若零红，说明当时机器太闲、本实验无效，必须加压重跑，
// **不能拿 A/B 的零红当结论**。
//
// 用法：build/dev/bin/pty_form_probe [每臂轮数] [臂子集: P|A|B|all]

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/core/pty.hpp"
#include "st/text/ansi_screen.hpp"
#include "st/ui/components/terminal.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Terminal;
using Clock = std::chrono::steady_clock;

constexpr const char* kPrompt = "st-test#";
constexpr const char* kMarker = "running-9271";

/// 一臂的形态：命令文本 + 是否等标记。
struct Arm {
  const char* label;
  const char* cmd;
  bool wait_marker;
};

constexpr Arm kArmPrefix{"P", "sleep 60", false};
constexpr Arm kArmTwo{"A", "echo 'ru''nning-9271'; sleep 60", true};
constexpr Arm kArmSameProc{"B", "/bin/sh -c \"echo 'ru''nning-9271'; exec sleep 60\"", true};

/// `/proc/loadavg` 的 1 分钟值（结论必须带负载条件）。
[[nodiscard]] auto load1() -> double {
  std::FILE* file = std::fopen("/proc/loadavg", "r");
  if (file == nullptr) return -1.0;
  double value = -1.0;
  if (std::fscanf(file, "%lf", &value) != 1) value = -1.0;
  std::fclose(file);
  return value;
}

struct Harness {
  st::ui::UiRoot root{};
  Terminal* terminal{nullptr};

  Harness() {
    root.set_viewport(st::math::Size{900.0f, 500.0f});
    auto owned = std::make_unique<Terminal>();
    terminal = owned.get();
    (void)setenv("PS1", "st-test# ", 1);
    terminal->set_shell("/bin/bash", {"--noprofile", "--norc", "-i"});
    root.set_content(std::move(owned));
    root.layout(true);
  }

  [[nodiscard]] auto screen_text() -> std::string {
    const auto* screen = terminal->screen(0);
    return screen != nullptr ? screen->plain_text() : std::string{};
  }

  template <typename Predicate>
  auto wait_until(Predicate predicate, int timeout_ms) -> bool {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
      terminal->pump();
      if (predicate()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  [[nodiscard]] auto prompt_count() -> std::size_t {
    const std::string text = screen_text();
    std::size_t count = 0;
    for (std::size_t at = text.find(kPrompt); at != std::string::npos;
         at = text.find(kPrompt, at + 1)) {
      ++count;
    }
    return count;
  }
};

/// 一轮的结果。
struct Outcome {
  bool prompt_back{false};   ///< Ctrl+C 后回到提示符（本用例的被测语义）
  long ms{-1};               ///< 发 Ctrl+C → 回到提示符
  long recover_ms{-1};       ///< 失败现场再发一次 → 恢复（-1 = 没恢复/不需要）
};

auto one_round(const Arm& arm) -> Outcome {
  Harness harness;
  harness.terminal->open_shell();
  if (!harness.wait_until([&] { return harness.prompt_count() >= 1; }, 5000)) {
    harness.terminal->send_stop();
    return {};
  }
  harness.terminal->send_bytes(std::string(arm.cmd) + "\r");
  // 前置条件：命令真的被 shell 收到了（看到命令行/标记出现在屏幕上）。
  // P 臂只等到「命令行回显」为止——那正是 2026-10-08 之前的写法。
  const char* needle = arm.wait_marker ? kMarker : arm.cmd;
  if (!harness.wait_until(
          [&] { return harness.screen_text().find(needle) != std::string::npos; }, 5000)) {
    harness.terminal->send_stop();
    return {};
  }

  const auto sent_at = Clock::now();
  harness.terminal->send_stop();
  Outcome out;
  out.prompt_back = harness.wait_until([&] { return harness.prompt_count() >= 2; }, 5000);
  out.ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sent_at).count();
  if (!out.prompt_back) {
    // 失败现场：再发一次。5 ms 内恢复 ⇒ 第一次被吞（不是会话僵住、不是产品缺陷）。
    const auto again_at = Clock::now();
    harness.terminal->send_stop();
    out.recover_ms = harness.wait_until([&] { return harness.prompt_count() >= 2; }, 3000)
                         ? std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                                again_at)
                               .count()
                         : -1;
  }
  harness.terminal->send_stop();
  return out;
}

/// 逐臂统计。
struct Tally {
  int rounds{0};
  int red{0};
  long green_sum{0};
  long green_worst{0};
  int green_count{0};
  std::vector<long> red_recover_ms{};
};

void print_tally(const Arm& arm, const Tally& tally, double load_before, double load_after) {
  st::print("  {:<2} 红 {}/{}（{:.0f}%）· 绿轮 ms：均 {} / 最差 {} · 负载 {:.2f} → {:.2f}\n",
            arm.label, tally.red, tally.rounds,
            tally.rounds > 0 ? 100.0 * tally.red / tally.rounds : 0.0,
            tally.green_count > 0 ? tally.green_sum / tally.green_count : 0L, tally.green_worst,
            load_before, load_after);
  if (!tally.red_recover_ms.empty()) {
    std::string detail;
    for (const long ms : tally.red_recover_ms) {
      detail += std::format(" {}ms", ms);
    }
    st::print("     红轮「再发一次 Ctrl+C 恢复」耗时：{}\n", detail);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const st::probe::spec spec{
      .title = "A/B 实验：`terminal_pty_stop_interrupts_without_killing_the_shell` 偶发红的因果与修法。",
      .criterion = "",  // 判据由人补（脚本不猜合格线）
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("pty_form_probe", spec, opts)) return st::probe::exit_ok;
  st::probe::result verdict{};

  const int per_arm = argc > 1 ? std::atoi(argv[1]) : 20;
  const std::string only = argc > 2 ? argv[2] : "all";
  if (!st::process::PtySession::supported()) {
    st::print("本环境不支持 PTY：探针不适用\n");
    return 0;
  }

  std::vector<Arm> arms;
  if (only == "all") {
    arms.push_back(kArmPrefix);
    arms.push_back(kArmTwo);
    arms.push_back(kArmSameProc);
  } else {
    for (const char c : only) {
      if (c == 'P') arms.push_back(kArmPrefix);
      if (c == 'A') arms.push_back(kArmTwo);
      if (c == 'B') arms.push_back(kArmSameProc);
    }
  }

  const double load_at_start = load1();
  st::print("每臂 {} 轮 · 臂={} · 起始负载 {:.2f}\n", per_arm, only, load_at_start);

  std::vector<Tally> tallies(arms.size());
  // **逐轮轮转**（round-major）而不是"一臂跑完再跑下一臂"：后者会把
  // 「先跑的那臂恰好赶上别的构建」变成系统性偏差，而本实验的因变量正是负载敏感度。
  for (int round = 0; round < per_arm; ++round) {
    for (std::size_t index = 0; index < arms.size(); ++index) {
      const Outcome out = one_round(arms[index]);
      Tally& tally = tallies[index];
      ++tally.rounds;
      if (out.prompt_back) {
        ++tally.green_count;
        tally.green_sum += out.ms;
        if (out.ms > tally.green_worst) tally.green_worst = out.ms;
      } else {
        ++tally.red;
        tally.red_recover_ms.push_back(out.recover_ms);
      }
    }
  }

  const double load_at_end = load1();
  st::print("\n结果（负载 {:.2f} → {:.2f}）：\n", load_at_start, load_at_end);
  for (std::size_t index = 0; index < arms.size(); ++index) {
    print_tally(arms[index], tallies[index], load_at_start, load_at_end);
  }
  // 结论退出码由外壳统一给出（0 合格 / 1 不合格 / 77 跳过）

  return st::probe::main_of(spec, opts, verdict);
}
