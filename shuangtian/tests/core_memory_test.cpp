// `st::memory` 平台抽象的基本契约。
//
// 为什么值得一条测试：这两个数是**诊断靠的量**——`BACKLOG` 的「大 Markdown 预览
// 首帧 58ms」就是靠缺页数区分“冷内存”与“抢 CPU”的。
// 若它在本平台静默返回 0（而调用方当成“零缺页”），整条诊断链会得出相反结论。
//
// 所以这条测试钉的是**单调性**（累计计数只增不减）而**不是具体数值**：
// 具体值取决于平台与运行时状态，写死必然脆。

#include "st/core/memory.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"

#include <memory>
#include <vector>

using st::memory::has_stats;
using st::memory::page_fault_count;
using st::memory::working_set_bytes;

ST_TEST(memory_stats_are_monotonic_and_non_negative) {
  if (!has_stats()) {
    ST_SKIP("本平台不支持内存统计（两个查询恒返回 0）");
    return;
  }

  const std::uint64_t faults_before = page_fault_count();
  const std::uint64_t resident_before = working_set_bytes();

  // 制造可观察的内存活动：分配 + 逐页触碰。
  // 用 `make_unique_for_overwrite` 而不是 `vector(n, 0)`——后者构造时就清零，
  // 缺页在分配阶段发生，看不出“触碰才触发缺页”这件事。
  constexpr std::size_t kPixels = 8U * 1024U * 1024U / 4U;   // 8 MB
  {
    auto buffer = std::make_unique_for_overwrite<std::uint32_t[]>(kPixels);
    for (std::size_t index = 0; index < kPixels; index += 16) {
      buffer[index] = static_cast<std::uint32_t>(index);
    }
  }

  const std::uint64_t faults_after = page_fault_count();
  const std::uint64_t resident_after = working_set_bytes();

  st::log::out("[memory] 缺页 {} → {}（+{}）· 工作集 {} → {} 字节\n", faults_before, faults_after,
            faults_after - faults_before, resident_before, resident_after);

  // ① 累计缺页只增不减（回绕/读失败会表现为减小或归零）。
  ST_CHECK(faults_after >= faults_before);
  // ② 触碰 8 MB 后应产生缺页（平台不支持时已在上面 SKIP）。
  ST_CHECK(faults_after > faults_before);
  // ③ 工作集非零（读不到才返回 0，而 has_stats() 已为真）。
  ST_CHECK(resident_after > 0);
}
