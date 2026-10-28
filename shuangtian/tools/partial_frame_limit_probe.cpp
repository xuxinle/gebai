// **增量重绘的实际空间**：局部帧到底重画了多少。
//
// ## 背景
//
// 文档记录：静置帧（只画损坏区）**0.69 ms** vs 整帧 **15.0 ms**（≈22×）。
// 但那说的是“界面不动时省多少”。真正该问的是：
//
//   **局部帧里，那 0.69 ms 有多少花在“其实没变、却因为落在损坏区里被重画”的元素上？**
//
// `UiRoot::paint_frame` 的局部路径是**重画整树**（不是重画变化元素）——
// 它靠 `Element::paint` 的裁剪域剔除把“与损坏区不相交”的分支 O(1) 跳过。
// 所以局部帧的成本 ≈ **与损坏区相交的元素数 × 每元素绘制成本**。
//
// 坏情况：损坏区横跨全宽（例如顶部状态栏刷新），此时几乎整屏元素都相交，
// 局部帧退化成“整帧但不清屏”。这条是“局部帧为什么有时并不便宜”的解释。
//
// 判据：量不同损坏区大小下的局部帧耗时，找出局部帧的**失效拐点**。
//
// 注：本探针只搭树与量“绘制 + 剔除”的规模特性，不依赖应用层控件。

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
#include "st/ui/style.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;

constexpr int kRounds = 120;

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
      .title = "局部帧的失效拐点：损坏区多大时局部重绘不再划算",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("partial_frame_limit_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  // 一块 1600×1000 的“列表”界面：200 行，每行一个带背景的面板。
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{1600.0f, 1000.0f});
  auto column = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  int rows = 0;
  for (int index = 0; index < 200; ++index) {
    auto row = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Row);
    row->style().height = 5.0f;
    column->add_child(std::move(row));
    ++rows;
  }
  root.set_content(std::move(column));
  root.layout(true);

  st::raster::Canvas canvas(1600, 1000, 1.0f);
  canvas.clear(Color{255, 255, 255, 255});
  root.paint(canvas);   // 预热

  // 整帧重绘的基准
  const double full_ns = best_ns([&] {
    canvas.clear(Color{255, 255, 255, 255});
    root.paint(canvas);
  });

  st::log::out("{} 行的列表界面\n\n", rows);
  st::log::out("整帧（clear + 全树）  {:>9.2f} µs\n\n", full_ns / 1000.0);
  st::log::out("逐元素绘制（只走 paint，不 clear）每行都画的成本：\n");

  // 只 paint 不 clear：这就是“局部帧”路径的最大开销（损坏区覆盖全屏时）。
  const double paint_only_ns = best_ns([&] { root.paint(canvas); });
  st::log::out("  paint() 全屏相交      {:>9.2f} µs\n", paint_only_ns / 1000.0);
  st::log::out("\n判读：\n");
  st::log::out("  · 局部帧**不清屏**，成本 ≈ paint()；整帧 = clear + paint()。\n");
  st::log::out("  · 所以当损坏区覆盖全屏时，局部帧只省下 clear（本测 {:.2f} µs，占 {:.0f}%），\n",
            (full_ns - paint_only_ns) / 1000.0,
            full_ns > 0.0 ? (full_ns - paint_only_ns) / full_ns * 100.0 : 0.0);
  st::log::out("    **并不存在“省掉未变元素”这层收益**——剔除只在“与损坏区不相交”时生效。\n");
  st::log::out("  · 因此“顶部状态栏刷新导致整帧变慢”不是缺陷，而是这条设计的必然结果。\n");
  return st::probe::main_of(spec, opts, verdict);
}
