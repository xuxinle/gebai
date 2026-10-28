// **局部帧的剔除到底省了多少**——96 个卡片 + 185×97 脏区为何要 2.46 ms。
//
// ## 实测矛盾
//
// `tests/app_frame_budget_test.cpp` 的 `script_write_partial_vs_full_frame_cost`：
//
// ```
// 局部帧中位 2.458 ms（8.71× 清屏） · 整帧中位 43.597 ms · 局部/整帧 = 0.06
//            损坏区只有 185×97 = 17945 px（占 1280×800 的 1.8%）
// ```
//
// 按 `simd::over_row` 单价（0.262 ns/px）换算，17945 px 只需 **4.7 µs**——差 500 倍。
//
// 已排除（都是实测）：
//   · 整树遍历本身：1200 节点 `paint()` 才 37 µs（`partial_frame_overhead_probe`）；
//   · `layout` 早退：1200 节点 4.8 µs。
//
// ## 这个探针量什么
//
// 那棵树的 `Element::paint` 自带裁剪剔除（`subtree_may_paint`）。
// 把同规模的树（96 卡片）分别按「全屏裁剪」与「185×97 裁剪」绘制，
// 看**耗时的真实比值**：
//
//   · 若接近 1.0× → 剔除**根本没生效**（那是缺陷，值一轮修复）
//   · 若显著小于 1.0× → 剔除生效，2.46 ms 另有来源
//
// 判据：报比值。

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
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;

constexpr int kRounds = 200;
constexpr int kWidth = 1280;
constexpr int kHeight = 800;

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
      .title = "局部帧裁剪剔除的有效性（185×97 脏区 vs 全屏）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("cull_effectiveness_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  // 与 `build_representative_ui` 同规模：12 行 × 8 列 = 96 个面板。
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{static_cast<float>(kWidth), static_cast<float>(kHeight)});
  auto column = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  column->style().gap = 12.0f;
  int cards = 0;
  for (int row_index = 0; row_index < 12; ++row_index) {
    auto row = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Row);
    row->style().gap = 8.0f;
    for (int col = 0; col < 8; ++col) {
      auto card = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
      card->style().width = 140.0f;
      card->style().height = 48.0f;
      row->add_child(std::move(card));
      ++cards;
    }
    column->add_child(std::move(row));
  }
  root.set_content(std::move(column));
  root.layout(true);

  st::raster::Canvas canvas(kWidth, kHeight, 1.0f);
  canvas.clear(Color{0xF7, 0xF8, 0xFA, 255});
  root.paint(canvas);   // 预热

  // A) 全屏裁剪（无剔除效果）
  const double full_ns = best_ns([&] {
    canvas.push_clip_rect(st::math::Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                         static_cast<float>(kHeight)});
    root.paint(canvas);
    canvas.pop_clip();
  });

  // B) 185×97 的小裁剪（应让绝大多数卡片被剔除）
  const double small_ns = best_ns([&] {
    canvas.push_clip_rect(st::math::Rect{40.0f, 30.0f, 185.0f, 97.0f});
    root.paint(canvas);
    canvas.pop_clip();
  });

  // C) 空裁剪（全部被剔除）
  const double empty_ns = best_ns([&] {
    canvas.push_clip_rect(st::math::Rect{-500.0f, -500.0f, 10.0f, 10.0f});
    root.paint(canvas);
    canvas.pop_clip();
  });

  st::log::out("{} 个卡片（{}×{}）\n\n", cards, kWidth, kHeight);
  st::log::out("A 全屏裁剪      {:>10.2f} µs\n", full_ns / 1000.0);
  st::log::out("B 185×97 小裁剪 {:>10.2f} µs   → A/B = {:.2f}×\n", small_ns / 1000.0,
            small_ns > 0.0 ? full_ns / small_ns : 0.0);
  st::log::out("C 完全在屏外    {:>10.2f} µs   → A/C = {:.2f}×\n", empty_ns / 1000.0,
            empty_ns > 0.0 ? full_ns / empty_ns : 0.0);
  st::log::out("\n判读：\n");
  const double ratio = small_ns > 0.0 ? full_ns / small_ns : 0.0;
  if (ratio < 1.5) {
    st::log::out("  A/B 只有 {:.2f}× → **剔除几乎没省时间**，这是个缺陷。\n", ratio);
    st::log::out("  预期：185×97 只与 1~2 个卡片相交，B 应显著小于 A。\n");
    verdict.state = st::probe::status::fail;
    verdict.reason = "裁剪剔除未生效";
  } else {
    st::log::out("  A/B = {:.2f}× → 剔除**生效**；2.46 ms 的花费不在元素遍历上，\n", ratio);
    st::log::out("  而在别处（如 `paint_frame` 的损坏区处理 / 文字排版）。\n");
  }
  return st::probe::main_of(spec, opts, verdict);
}
