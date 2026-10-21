// **未归因的 0.42 ms 是什么**——每元素遍历固定开销。
//
// ## 背景
//
// gallery 1600×1000 一帧的拆解（`ST_PAINT_PROFILE=1`）：
//
// | 项 | 耗时 |
// |---|---|
// | 原语合计（clear/rect/round_rect/path/stroke/shadow/text） | **2.97 ms** |
// | 本批绘制最小 | **3.40 ms** |
// | **未归因** | **0.42 ms（12%）** |
//
// 那 0.42 ms 不在任何 `OpScope` 里——即不在绘制原语内部。候选：
//   ① `Element::paint` 每元素的自有开销（可见性/剔除/裁剪栈/计数器）；
//   ② 遍历本身（子元素循环、虚函数派发）；
//   ③ 绘制期间的状态维护（`collect_animation_requests` 等）。
//
// 本探针量 ① 的量级下界：构造真实规模的树，只做**遍历而不绘制**，
// 看纯遍历要多久——若它接近 0.42 ms，那就说明“未归因”主要是树遍历本身，
// 而非某个可优化的具体环节。
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
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;

constexpr int kRounds = 300;

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
      .title = "未归因的 0.42 ms：树遍历与每元素固定开销",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("paint_traverse_cost_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::print("gallery 实测：{} 个元素、原语 2.97 ms、绘制 3.40 ms、未归因 0.42 ms\n\n",
            43);   // gallery 树自检报告 43 种 type，这里只作背景说明

  st::raster::Canvas canvas(1600, 1000, 1.0f);
  canvas.clear(Color{255, 255, 255, 255});

  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{1600.0f, 1000.0f});
  // 造与 gallery 相近规模的树（真实应用树通常是 100~300 节点）。
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

  // A) 整帧绘制（全量）
  const double paint_ns = best_ns([&] { root.paint(canvas); });
  // B) 只 layout（无损坏区时早退）
  const double layout_ns = best_ns([&] { root.layout(false); });

  st::print("{} 节点树：\n\n", made);
  st::print("整帧 paint()      {:>10.1f} µs\n", paint_ns / 1000.0);
  st::print("layout(早退)      {:>10.1f} µs\n", layout_ns / 1000.0);
  st::print("\n每元素平均        {:>10.2f} ns（paint）\n",
            paint_ns / static_cast<double>(made));
  st::print("\n判读：\n");
  st::print("  · 若每元素 ns 落在几十~几百，则“未归因 0.42 ms”≈ 树遍历 + 每元素固定开销；\n");
  st::print("  · 那是**难以消除**的（剔除判断、裁剪栈、虚派发都必要），\n");
  st::print("    除非减少元素数（如虚拟化长列表）——那是应用层选择，不是框架优化点。\n");
  return st::probe::main_of(spec, opts, verdict);
}
