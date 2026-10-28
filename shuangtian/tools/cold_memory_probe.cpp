// **首帧 58ms 的元凶是不是缺页**——验证 `BACKLOG` 里那条决定性线索。
//
// ## 背景（BACKLOG P1「大 Markdown 预览首次打开 ~58ms」）
//
// 已逐个排除 7 个方向（measure 自身、MarkdownView、终端/Git 面板、树规模、
// TextRenderer 构造、文本宽度缓存），决定性线索是：
//
//   同一帧内对同一子树连测两次 —— **首次 45.3ms、再测 0.2ms（差 226×）**。
//
// 那个比值说明不是任何算法/重复计算，而是**首次遍历的冷内存**或**后台线程抢 CPU**。
// BACKLOG 明确写了下一步该量什么（并注明“别先改代码”）。
//
// ## 本探针做什么
//
// 量「首次大块内存触碰」vs「第二次触碰」的成本比，并直接读**进程缺页计数**
// （经 `st::memory::page_fault_count()` 的平台抽象），从而把“冷内存”与
// “纯 CPU 慢”分开。
//
// ⚠ **缓冲必须用裸 `new`**：`std::vector(n, 0)` 的构造已经把内存清零一遍
// （= 已经触碰过），冷内存效应会被完全掩盖——实测那样量出来首次/二次都是
// 7 ms、缺页只有 2 次，会得出“本机没有冷热效应”的错误结论。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <vector>

#include "st/core/memory.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"

namespace {

void touch_all(std::span<std::uint32_t> buffer) {
  // 逐缓存行（64 字节 = 16 个 uint32）写一次，与光栅器填像素的访存模式近似。
  for (std::size_t index = 0; index < buffer.size(); index += 16) {
    buffer[index] = static_cast<std::uint32_t>(index);
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "首帧冷内存假设验证：缺页 vs 纯 CPU",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("cold_memory_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  if (!st::memory::has_stats()) {
    st::log::out("跳过：本平台不支持内存统计\n");
    return st::probe::main_of(spec, opts, verdict);
  }

  constexpr std::size_t kPixels = 64U * 1024U * 1024U / 4U;   // 64 MB
  st::log::out("缓冲 {} MB，逐缓存行触碰\n\n", kPixels * 4U / (1024U * 1024U));

  std::vector<double> cold_ms;
  std::vector<double> warm_ms;
  std::uint64_t cold_faults = 0;
  std::uint64_t warm_faults = 0;
  for (int round = 0; round < 8; ++round) {
    // **`make_unique_for_overwrite`**：默认初始化（不写入），即内存仍是未触碰页。
    // 用 `new` 会被 lint L1 拦（禁裸 new）；用 `std::vector(n, 0)` 则会把内存
    // 清零一遍（= 已经触碰），冷内存效应被完全掩盖。`make_unique_for_overwrite`
    // 是唯一同时满足“不触碰”与“不裸 new”的写法。
    auto raw = std::make_unique_for_overwrite<std::uint32_t[]>(kPixels);
    const std::span<std::uint32_t> fresh(raw.get(), kPixels);

    const auto before_cold = st::memory::page_fault_count();
    const auto cold_start = std::chrono::steady_clock::now();
    touch_all(fresh);
    const auto cold_end = std::chrono::steady_clock::now();
    cold_faults += st::memory::page_fault_count() - before_cold;
    cold_ms.push_back(std::chrono::duration<double, std::milli>(cold_end - cold_start).count());

    const auto before_warm = st::memory::page_fault_count();
    const auto warm_start = std::chrono::steady_clock::now();
    touch_all(fresh);
    const auto warm_end = std::chrono::steady_clock::now();
    warm_faults += st::memory::page_fault_count() - before_warm;
    warm_ms.push_back(std::chrono::duration<double, std::milli>(warm_end - warm_start).count());
  }

  const auto best = [](const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::min_element(values.begin(), values.end());
  };
  const double cold_best = best(cold_ms);
  const double warm_best = best(warm_ms);
  const double ratio = warm_best > 0.0 ? cold_best / warm_best : 0.0;

  st::log::out("首次触碰（新分配）：最小 {:.2f} ms · 缺页合计 {}\n", cold_best, cold_faults);
  st::log::out("第二次触碰（同块）：最小 {:.2f} ms · 缺页合计 {}\n", warm_best, warm_faults);
  st::log::out("\n耗时比（首次/第二次）= **{:.2f}×**\n", ratio);

  // 理论缺页数：64 MB / 4 KB 页 = 16384，8 轮约 131072。
  const std::uint64_t expected = static_cast<std::uint64_t>(kPixels * 4U / 4096U) * 8U;
  st::log::out("\n判读：\n");
  if (cold_faults > expected / 4U) {
    st::log::out("  首次缺页 {} 次（理论 ~{} 次）→ **冷内存确实是主因**\n", cold_faults, expected);
    st::log::out("  → 方向：预占 + 预碰（alloc-then-touch），别让主线程在首帧逐页触发缺页。\n");
    st::log::out("  ⚠ 但先看**绝对量**：`tools/cold_memory_scale_probe.cpp` 显示 600 KB 只需\n");
    st::log::out("    0.373 ms，而待解释的首帧是 58 ms——机制成立不等于值得优化。\n");
  } else if (ratio > 3.0) {
    st::log::out("  缺页相近但耗时差很多 → **不是缺页**，该查调度/抢 CPU/缓存\n");
  } else {
    st::log::out("  两者都接近 → 本机在这个尺度上看不到明显冷热效应\n");
  }
  return st::probe::main_of(spec, opts, verdict);
}
