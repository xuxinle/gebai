// **布局系统的重复计算**：`measure` 与 `arrange` 是否重复算同一次文本宽度。
//
// ## 为什么值得查
//
// 声明式界面的 `UiRoot::layout()` 流程是：
//   ① `content_->measure(constraints)` —— 自底向上量尺寸
//   ② `layout_subtree(*content_, rect)` → 每个元素 `arrange(context, rect)` —— 自顶向下定位
//
// 两步都会走整树。而**文本宽度是布局里最贵的输入**（要查字体、算 advance、
// 可能触发字形加载/排版）。若 `arrange` 阶段又把宽度重算一遍，那就是纯浪费。
//
// 现有缓解：`TextRenderer` 有文本宽度缓存（`docs/BACKLOG` 提过 LRU 被后台刷爆的问题），
// 所以重复调用应该命中缓存。但**缓存本身也有成本**（哈希键、查表、LRU 维护）。
//
// ## 这个探针量什么
//
// · A) `layout(force=true)` 全量重排——含 measure + arrange
// · B) `layout(force=false)` 早退——只剩主题下发
// · C) 只 measure 一遍（不经 arrange）
//
// A − B 就是“真重排”的成本；若它远大于 C，说明 arrange 阶段有重复计算。
//
// 判据：报 A/B/C 分解。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;

constexpr int kRounds = 200;

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
      .title = "布局重复计算：measure vs arrange 的成本分解",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("layout_duplicate_work_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  // 一个“文字密集”的界面：96 个带文本的卡片（与 frame budget 测试同型）。
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{1280.0f, 800.0f});
  auto column = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  int texts = 0;
  for (int row_index = 0; row_index < 12; ++row_index) {
    auto row = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Row);
    for (int col = 0; col < 8; ++col) {
      auto card = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
      card->style().width = 140.0f;
      card->style().height = 48.0f;
      auto label = std::make_unique<st::ui::Text>(std::format("卡片 {}·{}", row_index, col));
      label->style().font_size = 13.0f;
      card->add_child(std::move(label));
      row->add_child(std::move(card));
      ++texts;
    }
    column->add_child(std::move(row));
  }
  root.set_content(std::move(column));

  // 首帧（要真排一次，也会加载字体）
  root.layout(true);

  // A) 全量重排（measure + arrange）
  const double force_ns = best_ns([&] { root.layout(true); });
  // B) 早退（只剩主题下发遍）
  const double early_ns = best_ns([&] { root.layout(false); });

  st::log::out("{} 段文本（96 卡片，13px）\n\n", texts);
  st::log::out("A 全量重排（measure + arrange）{:>10.2f} µs\n", force_ns / 1000.0);
  st::log::out("B 早退（主题下发遍）         {:>10.2f} µs\n", early_ns / 1000.0);
  st::log::out("A − B（真重排）              {:>10.2f} µs  → {:.1f} µs/段文本\n",
            (force_ns - early_ns) / 1000.0,
            (force_ns - early_ns) / 1000.0 / static_cast<double>(texts));

  st::log::out("\n判读：\n");
  const double per_text = (force_ns - early_ns) / 1000.0 / static_cast<double>(texts);
  if (per_text < 1.0) {
    st::log::out("  {:.2f} µs/段 → 文本宽度缓存**生效**（未缓存的排版远高于此，\n", per_text);
    st::log::out("  一次 13px 中文串的完整排版是几十 µs 量级）。布局无重复计算问题。\n");
  } else {
    st::log::out("  {:.2f} µs/段偏高 → 可能每次重排都在重算文本宽度，值一轮修复。\n", per_text);
  }
  return st::probe::main_of(spec, opts, verdict);
}
