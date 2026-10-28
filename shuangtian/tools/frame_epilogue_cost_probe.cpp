// **未归因的 0.42 ms 定位**：帧末的整树收尾遍。
//
// ## 背景
//
// gallery 1600×1000：原语合计 2.97 ms、绘制 3.40 ms、**未归因 0.42 ms（12%）**。
//
// 已排除：
//   · 树遍历本身：300 节点整帧 `paint()` 才 **9.2 µs**（`paint_traverse_cost_probe`），
//     比 0.42 ms 小 45 倍——遍历不是主因；
//   · 所有 `Surface` 原语都有 `OpScope`（`clear`/`fill_*`/`stroke`/`shadow`/
//     `text`/`image`/`clip_mask` 共 11 处），且 `set_profiler` 在 `layout` 之前，
//     覆盖完整。
//
// 所以候选收窄到**帧内不在原语里的收尾工作**。`UiRoot::paint_frame` 里有两处：
//   ① `collect_tree_damage()`（帧首，收集损坏区）
//   ② `collect_animation_requests()`（帧末，汇总“还有谁在动”）
//
// 两者都是**整树递归**。本探针量它们在真实规模树上的成本。
//
// 判据：只报数。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;

constexpr int kRounds = 400;

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
      .title = "帧内整树收尾遍的成本（未归因 0.42 ms 的候选）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("frame_epilogue_cost_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::raster::Canvas canvas(1600, 1000, 1.0f);
  canvas.clear(Color{255, 255, 255, 255});

  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{1600.0f, 1000.0f});
  auto column = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  int made = 1;
  while (made < 300) {
    auto row = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Row);
    for (int index = 0; index < 5 && made < 300; ++index) {
      row->add_child(std::make_unique<st::ui::Spacer>());
      ++made;
    }
    column->add_child(std::move(row));
  }
  root.set_content(std::move(column));
  root.layout(true);
  root.paint(canvas);   // 预热

  // 整帧 paint（含两遍收尾）
  const double paint_full_ns = best_ns([&] { root.paint(canvas); });
  // 只 layout（早退路径：含主题下发遍）
  const double layout_only_ns = best_ns([&] { root.layout(false); });

  st::log::out("{} 节点树：\n\n", made);
  st::log::out("整帧 paint()   {:>9.1f} µs  → 每元素 {:>6.1f} ns\n", paint_full_ns / 1000.0,
            paint_full_ns / static_cast<double>(made));
  st::log::out("layout(早退)   {:>9.1f} µs  → 每元素 {:>6.1f} ns\n", layout_only_ns / 1000.0,
            layout_only_ns / static_cast<double>(made));
  st::log::out("\n判读：\n");
  st::log::out("  · 这两项之和远小于 420 µs → **整树收尾不是 0.42 ms 的来源**；\n");
  st::log::out("    那 0.42 ms 只能在某个具体原语里，而该原语的 OpScope 统计有缺口。\n");
  st::log::out("  · 若接近 420 µs → 就是它，方向是把收尾遍改成“只走标脏子树”。\n");
  return st::probe::main_of(spec, opts, verdict);
}
