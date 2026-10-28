// **同一个场景，两条路径给出 19 倍的耗时**——查清哪个是真的。
//
// ## 矛盾
//
// | 测法 | 结果 |
// |---|---|
// | `tests/app_frame_budget_test.cpp` 的 `script_write_partial_vs_full_frame_cost` | 局部帧 **2.458 ms**、整帧 **43.597 ms** |
// | `tools/frame_allocation_probe.cpp`（同型 96 卡片 + 文字，`root.paint()`） | 整帧 **129.6 µs** |
//
// 同是 96 卡片 + 13px 文字 + 1280×800，相差 **19 倍（局部）/ 336 倍（整帧）**。
//
// ## 三个候选差异
//
// 1. **`Card` vs `Panel`**：测试用 `st::ui::Card`（**带两层阴影**），我的探针用空 `Panel`。
//    而 `docs/PAINT_DIAGNOSIS.md` 记录 96 卡片阴影本身就是 ms 级——
//    **这最可能是主因**。
// 2. **`paint_frame()` vs `paint()`**：测试走 `paint_frame`（含损坏区收集、
//    动画汇总、可能的 `layout`），我用 `paint()`（只有绘制）。
// 3. **debug 档 vs release 档**：测试跑在 `st test`（**debug/-O0**），
//    我的探针是 `release/-O2`。这一条差距通常 2~4 倍，不足以解释 19 倍。
//
// ## 这个探针量什么
//
// 在**同一个二进制、同一个档位**里，把上面三项逐个打开，看耗时怎么涨：
//
//   A) 空 Panel，`paint()`
//   B) + Card（带阴影），`paint()`
//   C) + Card，`root.paint_frame()`
//
// 判据：找出 19 倍差距里各项各占多少。

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
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
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

/// `use_card` 为真时用 `st::ui::Card`（带两层阴影），否则用空 `Panel`。
[[nodiscard]] auto build_ui(bool use_card, int& labels) -> std::unique_ptr<st::ui::Element> {
  auto column = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  column->style().gap = 12.0f;
  labels = 0;
  for (int row_index = 0; row_index < 12; ++row_index) {
    auto row = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Row);
    row->style().gap = 8.0f;
    for (int col = 0; col < 8; ++col) {
      std::unique_ptr<st::ui::Element> card;
      if (use_card) {
        auto instance = std::make_unique<st::ui::Card>();
        instance->style().width = 140.0f;
        instance->style().height = 48.0f;
        card = std::move(instance);
      } else {
        auto instance = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
        instance->style().width = 140.0f;
        instance->style().height = 48.0f;
        card = std::move(instance);
      }
      auto label = std::make_unique<st::ui::Text>(std::format("卡片 {}·{}", row_index, col));
      label->style().font_size = 13.0f;
      card->add_child(std::move(label));
      row->add_child(std::move(card));
      ++labels;
    }
    column->add_child(std::move(row));
  }
  return column;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "19 倍差异归因：Card 阴影 vs Panel、paint_frame vs paint",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("frame_variance_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  struct Case {
    const char* name;
    bool card;
    bool frame;
  };
  const Case cases[] = {
      {"A 空 Panel + paint()", false, false},
      {"B Card（带阴影）+ paint()", true, false},
      {"C Card + paint_frame()", true, true},
  };

  st::log::out("96 卡片 + 13px 文字，1280×800\n\n");
  st::log::out("{:<32}{:>14}{:>14}\n", "场景", "耗时", "相对 A");

  double first_ns = 0.0;
  for (const Case& item : cases) {
    int labels = 0;
    st::ui::UiRoot root;
    root.set_viewport(st::math::Size{1280.0f, 800.0f});
    root.set_content(build_ui(item.card, labels));
    root.layout(true);

    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(1280, 800, 1.0f);
    canvas.clear(Color{0xF7, 0xF8, 0xFA, 255});
    root.paint(canvas);   // 预热

    const double ns = best_ns([&] {
      canvas.clear(Color{0xF7, 0xF8, 0xFA, 255});
      if (item.frame) {
        (void)root.paint_frame(canvas);
        root.clear_dirty();
      } else {
        root.paint(canvas);
      }
    });
    if (first_ns == 0.0) first_ns = ns;
    st::log::out("{:<32}{:>11.1f} µs{:>13.2f}×\n", item.name, ns / 1000.0,
              first_ns > 0.0 ? ns / first_ns : 0.0);
  }

  st::log::out("\n参考（debug 档 `st test`）：局部帧 2458 µs / 整帧 43597 µs\n");
  st::log::out("\n判读：\n");
  st::log::out("  · 若 B ≫ A → 差距主体是 **Card 的两层阴影**（与 PAINT_DIAGNOSIS 一致）；\n");
  st::log::out("  · 若 C ≈ B → `paint_frame` 的额外工作可忽略；\n");
  st::log::out("  · 剩余差距（release vs debug）通常是 2~4 倍，不是 19 倍。\n");
  return st::probe::main_of(spec, opts, verdict);
}
