// 字体栈「第二次调用」的成本分解 —— 为 tests/text_font_default_test.cpp 的
// 「缓存生效」断言定位真因（那断言原为 `hot_ms < 1.0`，实测稳定 ~172 ms 失败）。
//
// 用法：pwsh -File tools/build_probes.ps1 -Only font_stack_cost_probe
//       build/probe/font_stack_cost_probe.exe
//
// 只报数，不下结论：逐段计时，让「候选链 / 文件探测 / 解析 / FontStack 构造」各自现形。
// 最小值而非均值：均值会被一次杀毒扫描整段带偏（本机就是这个用例红的）。

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/font_platform.hpp"
#include "st/core/print.hpp"
#include "st/text/text.hpp"

namespace {

using Clock = std::chrono::steady_clock;

template <typename Fn>
double best_ms(int rounds, Fn&& fn) {
  double best = 1e9;
  for (int i = 0; i < rounds; ++i) {
    const auto t0 = Clock::now();
    fn();
    const auto t1 = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (ms < best) best = ms;
  }
  return best;
}

}  // namespace

int main() {
  auto boot = st::text::FontStack::system_default();
  if (!boot) {
    st::print("无字体环境：本探针不适用\n");
    return 0;
  }

  const auto text = st::platform::preferred_text_fonts();
  const auto bold = st::platform::preferred_text_fonts_bold();
  const auto mono = st::platform::preferred_mono_fonts();
  st::print("候选数：正文 {} · 粗体 {} · 等宽 {}\n", text.size(), bold.size(), mono.size());
  for (const auto& e : text) {
    st::print("  正文候选 cjk={}  {}\n", e.cjk ? 1 : 0, e.path);
  }

  // ① 纯平台层：候选链枚举（内含每条的 is_regular_file）
  const double platform_ms = best_ms(20, [&] {
    volatile std::size_t sink = 0;
    sink += st::platform::preferred_text_fonts().size();
    sink += st::platform::preferred_text_fonts_bold().size();
    sink += st::platform::preferred_mono_fonts().size();
    (void)sink;
  });

  // ② 文件存在性探测：候选链上每个路径各查一次
  const double exists_ms = best_ms(20, [&] {
    volatile int sink = 0;
    for (const auto& e : text) sink += st::fs::is_regular_file(e.path) ? 1 : 0;
    for (const auto& e : bold) sink += st::fs::is_regular_file(e.path) ? 1 : 0;
    for (const auto& e : mono) sink += st::fs::is_regular_file(e.path) ? 1 : 0;
    (void)sink;
  });

  // ③ 完整 FontStack::system_default()（缓存已热）
  const double hot_ms = best_ms(20, [] {
    volatile bool ok = st::text::FontStack::system_default().has_value();
    (void)ok;
  });

  st::print("\n");
  st::print("① 平台层候选链枚举       {:>8.3f} ms\n", platform_ms);
  st::print("② 仅文件存在性探测       {:>8.3f} ms\n", exists_ms);
  st::print("③ system_default（热）   {:>8.3f} ms   ← 原断言要求 < 1.0\n", hot_ms);
  st::print("   ③ − ① =              {:>8.3f} ms\n", hot_ms - platform_ms);
  return 0;
}
