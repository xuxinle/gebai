// **绘制路径的分配压力**——用「重复绘制的耗时稳定性」间接量。
//
// ## 为何不直接数分配次数
//
// 覆写全局 `operator new` 记账是标准做法，但 **GCC 的
// `-Werror=mismatched-new-delete`** 会把“在 operator new 里 malloc”当成缺陷
// （该诊断无法区分“自定义分配器”与“真的不匹配”）。而本仓库 `-Werror` 是硬约束。
//
// ## 间接判据：耗时离散度
//
// 每帧大量堆分配会带来两个可观测后果：
//   ① 分配器状态波动 → 同样的绘制耗时离散度变大（p95/p50 比值高）；
//   ② 长时间运行后碎片累积 → 后段帧比前段慢。
//
// 本探针量同一帧重复 400 次的耗时分布：
//   · p95/p50 接近 1 → 分配压力小（分配器处于稳定态）；
//   · p95/p50 明显 > 1.2 → 有分配抖动，值查逐帧分配。
//
// 另附“前 100 次 vs 后 100 次”的比值，用来看有无碎片累积趋势。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
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

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "一帧绘制的堆分配次数与字节数",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("frame_allocation_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{1280.0f, 800.0f});
  auto column = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  int labels = 0;
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
      ++labels;
    }
    column->add_child(std::move(row));
  }
  root.set_content(std::move(column));
  root.layout(true);

  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(1280, 800, 1.0f);
  canvas.clear(Color{0xF7, 0xF8, 0xFA, 255});
  root.paint(canvas);   // 预热

  // ① 重复绘制同一帧，收集耗时分布
  constexpr int kFrames = 400;
  std::vector<double> samples;
  samples.reserve(kFrames);
  for (int frame = 0; frame < kFrames; ++frame) {
    const auto start = std::chrono::steady_clock::now();
    canvas.clear(Color{0xF7, 0xF8, 0xFA, 255});
    root.paint(canvas);
    const auto end = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
  }

  std::vector<double> sorted = samples;
  std::ranges::sort(sorted);
  const double p50 = sorted[static_cast<std::size_t>(kFrames * 0.50)];
  const double p95 = sorted[static_cast<std::size_t>(kFrames * 0.95)];
  const double p05 = sorted[static_cast<std::size_t>(kFrames * 0.05)];

  // ② 前 1/4 vs 后 1/4 的中位（看有无碎片累积趋势）
  const auto median_of = [](std::vector<double> values) {
    std::ranges::sort(values);
    return values[values.size() / 2];
  };
  const double early = median_of(std::vector<double>(samples.begin(), samples.begin() + 100));
  const double late = median_of(std::vector<double>(samples.end() - 100, samples.end()));

  st::log::out("{} 段文本的界面（1280×800），重复绘制 {} 帧\n\n", labels, kFrames);
  st::log::out("p05 {:>7.1f} µs · p50 {:>7.1f} µs · p95 {:>7.1f} µs\n", p05, p50, p95);
  st::log::out("p95/p50 = {:.2f}×   ·   p05/p50 = {:.2f}×\n", p95 / p50, p05 / p50);
  st::log::out("前 100 帧中位 {:>7.1f} µs · 后 100 帧中位 {:>7.1f} µs → {:.2f}×\n", early, late,
            early > 0.0 ? late / early : 0.0);

  st::log::out("\n判读：\n");
  const double spread = p95 / p50;
  if (spread < 1.20 && early > 0.0 && late / early < 1.10) {
    st::log::out("  离散度 {:.2f}×（<1.2）且前后段无趋势 → **分配压力小**，分配器处于稳定态，\n",
              spread);
    st::log::out("  绘制路径没有明显的逐帧堆分配抖动。\n");
  } else {
    st::log::out("  离散度 {:.2f}×或前后段有趋势 → 有分配抖动/碎片累积，\n", spread);
    st::log::out("  值查逐帧堆分配（向量扩容 / 临时 string / 每元素分配）。\n");
  }
  return st::probe::main_of(spec, opts, verdict);
}
