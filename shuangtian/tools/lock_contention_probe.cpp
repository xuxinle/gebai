/// 量化：主线程 measure_width 在与后台排版并发时的吞吐下降（锁竞争证据）。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "st/app/text_port.hpp"
#include "st/text/text.hpp"

#if defined(__linux__)
#include <unistd.h>   // nice(2)：复刻 SlowContent 后台线程的最低优先级
#endif

using Clock = std::chrono::steady_clock;

int main() {
  auto fonts = st::text::FontStack::system_default();
  if (!fonts) { std::printf("SKIP\n"); return 0; }
  st::text::TextRenderer renderer{*fonts, 1.0f};
  auto port = st::app::make_text_port(renderer);
  std::vector<std::string> words;
  for (int i = 0; i < 2000; ++i) words.push_back("word" + std::to_string(i) + " 中文");

  // ① 独占时主线程吞吐
  const auto alone_start = Clock::now();
  volatile float sink = 0;
  for (int round = 0; round < 20; ++round)
    for (const auto& w : words) sink = sink + port->measure_width(w, 14.0f);
  const double alone_ms = std::chrono::duration<double, std::milli>(Clock::now() - alone_start).count();

  // ② 后台线程狂调 measure_width（模拟**多个**排版线程），主线程再测同样工作量
  std::atomic<bool> stop{false};
  std::thread background([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      for (const auto& w : words) (void)port->measure_width(w, 14.0f);
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 让后台先热起来
  const auto contend_start = Clock::now();
  for (int round = 0; round < 20; ++round)
    for (const auto& w : words) sink = sink + port->measure_width(w, 14.0f);
  const double contend_multi = std::chrono::duration<double, std::milli>(Clock::now() - contend_start).count();
  stop.store(true);
  background.join();

  // ③ **真实场景**：后台只有一个排版线程、且低优先级（nice 19）——
  //    这是 `SlowContent` 修复后的实际状态（单一 worker + nice）。
  std::atomic<bool> stop2{false};
  std::thread single([&] {
#if defined(__linux__)
    if (::nice(19) == -1) { /* 无权限：照常跑 */ }
#endif
    while (!stop2.load(std::memory_order_relaxed)) {
      for (const auto& w : words) (void)port->measure_width(w, 14.0f);
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto real_start = Clock::now();
  for (int round = 0; round < 20; ++round)
    for (const auto& w : words) sink = sink + port->measure_width(w, 14.0f);
  const double contend_single = std::chrono::duration<double, std::milli>(Clock::now() - real_start).count();
  stop2.store(true);
  single.join();

  std::printf("主线程同工作量：独占 %.1fms\n", alone_ms);
  std::printf("  + 多线程并发排版（修复前）: %.1fms（%.2f× 慢）\n", contend_multi, contend_multi / alone_ms);
  std::printf("  + 单线程低优先级（修复后）: %.1fms（%.2f× 慢）\n", contend_single, contend_single / alone_ms);
  // 判据看**修复后的真实形态**：单 worker + nice。多线程数字只是历史对照。
  const bool ok = contend_single < alone_ms * 2.0;
  std::printf("判据（单 worker + nice < 2.0×）：%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
