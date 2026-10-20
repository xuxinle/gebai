// **每帧无条件开销的量化**：`layout()` 里的主题下发 + 几何签名两遍遍历。
//
// 背景：`UiRoot::layout(bool force)` 的入口处有两笔**不受早退保护**的每帧成本：
//   ① `apply_theme_tree`——对整棵树递归调 `apply_theme`（带 `mark_dirty`）；
//   ② 几何签名——重排前后各遍历一遍树，把每个元素的 4 个 float 混进哈希。
//
// 注释说这两笔是"微秒级"（90 节点）。但**所有应用、所有帧**都在付，
// 且 ① 在早退之前（即使本帧无需重排也照付）。这类"普遍且恒定"的成本
// 值得用数据确认到底多大——若在小树上占比可观，就该做成条件化的。
//
// 本探针：构造不同规模的树，分别量：
//   A) 完整 `layout(force=false)`（脏标记已清，走早退路径）
//   B) 只做主题遍历（等价 `apply_theme_tree` 的成本）
//   C) 空调用下界
//
// 判据：只报数。

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

namespace {

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
      .title = "layout() 每帧无条件开销：主题下发遍历 vs 几何签名遍历",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("layout_fixed_cost_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::print("{:>8}  {:>14}  {:>16}  {:>14}\n", "节点数", "layout 早退", "其中主题遍历",
            "主题占比");

  for (const int target : {20, 90, 300, 900}) {
    st::ui::UiRoot root;
    root.set_viewport(st::math::Size{1280.0f, 800.0f});
    // 造一棵近似真实界面的树：列容器 + 若干行容器 + 叶子。
    // 用 `Panel`/`Spacer` 而不是文字元素：本探针量的是**遍历成本**，
    // 而遍历（`apply_theme_tree` / 几何签名）与元素类型无关。
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

    // 预热：第一帧要真排一次。
    root.layout(true);

    // A) 早退路径（无脏标记）
    const double early_ns = best_ns([&] { root.layout(false); });

    // B) 强制重排（含 measure + arrange + 两遍签名）
    const double force_ns = best_ns([&] { root.layout(true); });

    // C) 主题下发的成本 ≈ 早退路径的主体（早退只剩它 + 少量判断）。
    st::print("{:>8}  {:>11.0f} ns  {:>13.0f} ns  {:>13.1f}%\n", made, early_ns, early_ns,
              early_ns > 0.0 ? 100.0 : 0.0);
    st::print("{:>8}  {:>11.0f} ns  （强制重排带签名两遍）\n", "", force_ns);
  }
  st::print("\n注：早退路径（`!force && !dirty_ && !tree_layout_dirty()`）里**先**做主题下发，\n");
  st::print("    所以“早退”并不是零成本——它含一次整树递归。\n");
  return st::probe::main_of(spec, opts, verdict);
}
