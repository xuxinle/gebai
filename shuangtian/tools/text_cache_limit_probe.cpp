// **文本宽度缓存的实际效果**：命中率与失效模式。
//
// ## 为什么这个缓存是关键
//
// 声明式界面每帧都要 `measure` 整棵树，而文本宽度是布局里最贵的输入
//（要查字体、算 advance，未缓存时可能触发字形加载）。
// `layout_duplicate_work_probe` 已证 96 段文本全量重排只要 23.4 µs
//（0.23 µs/段）——**说明缓存命中良好**。
//
// 但缓存有个已知风险（`docs/BACKLOG` 记过）：**LRU 被后台排版刷爆**
//（实测 2.2~2.9× 退化）。所以要知道：
//
//   ① 缓存的上限是多少？
//   ② 超过上限后性能如何退化？
//   ③ 真实界面（几百段不同文本）会不会碰到上限？
//
// ## 这个探针量什么
//
// 用**互不相同**的文本填满测量，看耗时随“不同文本数”怎么变：
//   · 若在某个数量后**突然变慢** → 那是缓存上限，超过就失效；
//   · 若线性增长 → 没有上限问题（或上限很大）。
//
// 判据：找出拐点。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

namespace {

using st::math::Color;

constexpr int kRounds = 60;

[[nodiscard]] auto best_ns(const std::function<void()>& body) -> double {
  double best = 1e18;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto end = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(end - start).count();
    if (ns < best) best = ns;
  }
  return best;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "文本宽度缓存的容量拐点（超过多少条不同文本会失效）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("text_cache_limit_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  auto stack = st::text::FontStack::system_default();
  if (!stack.has_value()) {
    st::log::out("跳过：拿不到系统字体\n");
    return st::probe::main_of(spec, opts, verdict);
  }
  st::text::TextRenderer renderer(*stack, 1.0f);

  st::log::out("量 `measure` 随「不同文本条数」的耗时（13px 中文混排）\n\n");
  st::log::out("{:>10}{:>16}{:>16}\n", "不同文本数", "单次 measure", "相对 16 条");

  double baseline_ns = 0.0;
  for (const int count : {16, 64, 256, 1024, 4096}) {
    // 造 count 条互不相同的文本
    std::vector<std::string> texts;
    texts.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
      texts.push_back(std::format("文本条目{}号样例", index));
    }

    // 预热：让这批文本全部进缓存（若容量足够）
    for (const auto& text : texts) {
      (void)renderer.measure(text, 13.0f);
    }

    // 稳态：反复量同一批（若缓存容量 < count，会有持续的换入换出）
    const double total_ns = best_ns([&] {
      for (const auto& text : texts) {
        const auto metrics = renderer.measure(text, 13.0f);
        (void)metrics;
      }
    });
    const double per_ns = total_ns / static_cast<double>(count);
    if (baseline_ns == 0.0) baseline_ns = per_ns;

    st::log::out("{:>10}{:>11.2f} µs{:>15.2f}×\n", count, per_ns / 1000.0,
              baseline_ns > 0.0 ? per_ns / baseline_ns : 0.0);
  }

  st::log::out("\n判读：\n");
  st::log::out("  · 若单次耗时随条数**大幅上升** → 缓存有上限，真实界面超过后会退化；\n");
  st::log::out("  · 若基本持平（<1.5×）→ 缓存容量足够，无拐点，不需要调参。\n");
  st::log::out("\n参考：`docs/BACKLOG` 记过「LRU 被后台排版刷爆（2.2~2.9×）」——\n");
  st::log::out("      那是**多线程争用同一缓存**的情形，与本探针的单线程稳态不同。\n");
  return st::probe::main_of(spec, opts, verdict);
}
