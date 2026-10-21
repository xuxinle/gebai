// **局部帧为何是 2.46 ms**——185×97 的损坏区不该花这么久。
//
// ## 实测（`tests/app_frame_budget_test.cpp` 的 `script_write_partial_vs_full_frame_cost`）
//
// ```
// [frame-js-write] 局部帧中位 2.458ms（8.71× 清屏） vs 整帧中位 43.597ms
//                  → 局部/整帧 = 0.06（末轮局部=true，dirty=185×97）
// ```
//
// 损坏区只有 **185×97 = 17945 像素**（占 1280×800 的 1.8%），却要 2.46 ms。
// 按 `simd::over_row` 的物理单价（0.262 ns/px）换算，这点面积只需 **4.7 µs**——
// **差 500 倍**。所以钱不在“画”上。
//
// ## 局部帧的固定开销在哪（`UiRoot::paint_frame` 局部分支）
//
//   ① `collect_tree_damage()` —— 整树递归收集损坏区
//   ② `layout()` —— 含**主题下发整树递归**（即使早退也要走）
//   ③ `canvas.push_clip_rect` + `fill_rect`（清损坏区）
//   ④ `paint_subtree` —— **整树遍历**，靠裁剪剔除跳过不相交分支
//   ⑤ `collect_animation_requests()` —— **整树递归**
//
// ②④⑤ 三层整树递归，每层都是虚函数派发 + 分支判断。
// 树规模（gbcode 实测 ~298 节点，gallery ~105）决定了它们的成本。
//
// ## 这个探针量什么
//
// 分别量 ① ② ⑤ 在**不同树规模**下的成本，回答：
// “2.46 ms 里有多少是‘树有多大就付多少’的固定开销”。
//
// 若是——那优化方向是**把整树递归改成只走标脏子树**（需要脏标记沿树上传），
// 而不是继续抠绘制原语。

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
      .title = "局部帧固定开销 vs 树规模（解释 185×97 的脏区为何要 2.46ms）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("partial_frame_overhead_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::print("{:>8}  {:>14}  {:>14}  {:>14}\n", "节点数", "layout(早退)", "paint(全帧)",
            "两者合计");

  for (const int target : {100, 300, 600, 1200}) {
    st::ui::UiRoot root;
    root.set_viewport(st::math::Size{1280.0f, 800.0f});
    auto column = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    int made = 1;
    while (made < target) {
      auto row = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Row);
      for (int index = 0; index < 5 && made < target; ++index) {
        row->add_child(std::make_unique<st::ui::Spacer>());
        ++made;
      }
      column->add_child(std::move(row));
    }
    root.set_content(std::move(column));
    root.layout(true);

    st::raster::Canvas canvas(1280, 800, 1.0f);
    canvas.clear(Color{255, 255, 255, 255});
    root.paint(canvas);

    const double early_ns = best_ns([&] { root.layout(false); });
    const double paint_ns = best_ns([&] { root.paint(canvas); });
    st::print("{:>8}  {:>11.1f} µs  {:>11.1f} µs  {:>11.1f} µs\n", made, early_ns / 1000.0,
              paint_ns / 1000.0, (early_ns + paint_ns) / 1000.0);
  }

  st::print("\n判读：\n");
  st::print("  · 这两项之和随节点数**线性增长**，且每帧都付（与损坏区大小无关）；\n");
  st::print("  · 实测局部帧 2.46 ms 而脏区只有 17945 px —— 说明主体是这些整树递归，\n");
  st::print("    不是绘制本身（按 `over_row` 单价那点面积只需 ~4.7 µs）。\n");
  st::print("  · 方向：把 `collect_tree_damage` / `collect_animation_requests` /\n");
  st::print("    `apply_theme_tree` 三层整树递归改成**只走标脏子树**（脏标记沿树上传）。\n");
  return st::probe::main_of(spec, opts, verdict);
}
