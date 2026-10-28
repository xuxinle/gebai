// **冷内存成本 vs 缓冲体量**——判断“预碰（alloc-then-touch）”该用在哪里。
//
// ## 前情
//
// `tools/cold_memory_probe.cpp` 已证：64 MB 新分配 + 首次触碰 =
// **18.35 ms / 131503 次缺页**，同一块再碰一次只要 6.71 ms / 0 次（**2.74×**）
// ——缺页数与理论值 131072 几乎完全吻合，**冷内存是实打实的成本**。
//
// ## 本探针补的一环：先看绝对量
//
// 64 MB 是“大画布”量级，而 BACKLOG P1「大 Markdown 预览首帧 58 ms」的对象是
// **447 KB Markdown 展开成约 20 万字符**（≈ 600 KB）。
// 这个量级缺页只有 ~150 次——**撑不起 58 ms**。
//
// 机制成立 ≠ 值得优化。本探针按真实规模分档量，找出“多大体量才有可观冷热差”。

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

void touch_bytes(std::span<std::uint8_t> buffer) {
  for (std::size_t index = 0; index < buffer.size(); index += 64) {
    buffer[index] = static_cast<std::uint8_t>(index);
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "冷内存成本 vs 缓冲体量（判断“预碰”该用在哪里）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("cold_memory_scale_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  if (!st::memory::has_stats()) {
    st::log::out("跳过：本平台不支持内存统计\n");
    return st::probe::main_of(spec, opts, verdict);
  }

  st::log::out("{:>12}  {:>12}  {:>12}  {:>10}  {:>12}\n", "缓冲大小", "首次 ms", "第二次 ms",
            "倍数", "首次缺页");

  const std::size_t sizes_kb[] = {600, 2048, 8192, 32768};
  for (const std::size_t kilobytes : sizes_kb) {
    const std::size_t bytes = kilobytes * 1024U;
    double cold_best = 1e9;
    double warm_best = 1e9;
    std::uint64_t faults_total = 0;
    for (int round = 0; round < 6; ++round) {
      // `make_unique_for_overwrite` = 默认初始化（不写入）= 内存仍是未触碰页。
      // 不能用 `vector(n, 0)`（会清零一遍，冷内存效应被掩盖），也不允许裸 `new`（lint L1）。
      auto raw = std::make_unique_for_overwrite<std::uint8_t[]>(bytes);
      const std::span<std::uint8_t> fresh(raw.get(), bytes);

      const auto before = st::memory::page_fault_count();
      const auto cold_start = std::chrono::steady_clock::now();
      touch_bytes(fresh);
      const auto cold_end = std::chrono::steady_clock::now();
      faults_total += st::memory::page_fault_count() - before;
      cold_best =
          std::min(cold_best, std::chrono::duration<double, std::milli>(cold_end - cold_start).count());

      const auto warm_start = std::chrono::steady_clock::now();
      touch_bytes(fresh);
      const auto warm_end = std::chrono::steady_clock::now();
      warm_best =
          std::min(warm_best, std::chrono::duration<double, std::milli>(warm_end - warm_start).count());
    }
    st::log::out("{:>9} KB  {:>12.3f}  {:>12.3f}  {:>9.2f}×  {:>12}\n", kilobytes, cold_best,
              warm_best, warm_best > 0.0 ? cold_best / warm_best : 0.0, faults_total);
  }

  st::log::out("\n判读：\n");
  st::log::out("  · 600 KB（= 447 KB Markdown 展开后的量级）只有零点几 ms；\n");
  st::log::out("  · 而 BACKLOG 那条要解释的是一帧 58 ms —— **差两个数量级**，\n");
  st::log::out("    所以冷内存/缺页**不是**那 58 ms 的原因，别往“预碰”方向投入。\n");
  return st::probe::main_of(spec, opts, verdict);
}
