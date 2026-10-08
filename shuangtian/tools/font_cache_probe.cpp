// 验证 `font_cache_stats()` 对「缓存是否真的生效」有判别力。
//
// 用法：pwsh -File tools/build_probes.ps1 -Only font_cache_probe
//       build/probe/font_cache_probe.exe
//
// 判据（确定量，与机器快慢无关）：
//  ① 第一次 system_default 之后，后续每次必须**零新增 load / 零新增 count**（纯命中）；
//  ② 命中数必须真的在涨——否则 ① 会被“什么都不做”满足（假绿）。
// 这正是要替掉 `hot_ms < 1.0` 的那个量：耗时阈值分不清“缓存没命中”与“机器很忙”。

#include <cstddef>

#include "st/core/print.hpp"
#include "st/text/font.hpp"
#include "st/text/text.hpp"

namespace {

using st::text::font_cache_stats;

}  // namespace

int main() {
  // 预热：把系统字体链全部装进缓存
  if (!st::text::FontStack::system_default()) {
    st::print("无字体环境：本探针不适用\n");
    return 0;
  }
  const auto warm = font_cache_stats();
  st::print("预热后              loads={} hits={} counts={}\n", warm.face_loads,
            warm.face_hits, warm.count_reads);

  bool ok = true;
  bool hits_grew = false;
  for (int round = 0; round < 5; ++round) {
    auto stack = st::text::FontStack::system_default();
    if (!stack) {
      st::print("第 {} 轮 system_default 失败\n", round);
      return 1;
    }
    const auto now = font_cache_stats();
    const std::size_t d_load = now.face_loads - warm.face_loads;
    const std::size_t d_count = now.count_reads - warm.count_reads;
    const std::size_t d_hit = now.face_hits - warm.face_hits;
    if (d_hit > 0) hits_grew = true;
    st::print("第 {} 次热调用           load 增 {} · count 增 {} · hit 增 {}\n", round,
              d_load, d_count, d_hit);
    if (d_load != 0 || d_count != 0) ok = false;
  }

  st::print("\n判据 ①：热调用零新增 load/count → {}\n", ok ? "成立" : "**不成立（缓存没生效）**");
  st::print("判据 ②：命中数确实在涨（防假绿） → {}\n", hits_grew ? "成立" : "**不成立**");

  const auto final_stats = font_cache_stats();
  st::print("累计：loads={} hits={} counts={}\n", final_stats.face_loads,
            final_stats.face_hits, final_stats.count_reads);
  return (ok && hits_grew) ? 0 : 1;
}
