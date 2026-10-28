// **逐次记录 `fill_rect(圆角)` 的单次成本**，按尺寸分桶——找出是哪一类拖后腿。
//
// 背景：gallery 1600×1000 实测 `round_rect` = **33 次 / 1.59 ms**（整帧 37%），
// 而理论下界（`frame_floor_probe` 的多层混合）是 0.19 ns/px：
//   1.11 M 像素 × 0.19 ns/px ≈ **0.21 ms**，实际却是 **7.6 倍**。
//
// 而 `round_rect_cost_probe` 显示成本随面积强烈变化（小形状每像素贵 13 倍），
// 所以“平均 1.32 ns/px”这个数会掩盖真实分布：很可能是**大量小控件**在拖，
// 而不是少数大面板。
//
// 本探针按面积分桶统计「次数 / 总耗时 / ns per px」，直接看出该优化哪一档。
//
// 判据：只报数。

#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Paint;

constexpr int kWidth = 1600;
constexpr int kHeight = 1000;

/// 面积分桶：小控件 / 中卡片 / 大面板。
[[nodiscard]] auto bucket_of(double area) -> const char* {
  if (area < 5000.0) return "小 <5k px（按钮/标签）";
  if (area < 50000.0) return "中 5k~50k（卡片/输入框）";
  return "大 >50k（面板/编辑区）";
}

struct Bucket {
  int calls{0};
  double total_ms{0.0};
  double total_px{0.0};
};

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "圆角矩形按尺寸分桶的成本（找出拖后腿的是哪一档）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("round_rect_bucket_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  // 模拟真实界面里的一批圆角绘制（尺寸取自 gallery/gbcode 的常见控件）。
  struct Item {
    float width;
    float height;
    float radius;
  };
  const std::vector<Item> items = {
      // 小：按钮、标签、徽标
      {72.0f, 28.0f, 6.0f},   {72.0f, 28.0f, 6.0f},   {72.0f, 28.0f, 6.0f},
      {96.0f, 32.0f, 6.0f},   {96.0f, 32.0f, 6.0f},   {48.0f, 24.0f, 12.0f},
      {120.0f, 30.0f, 8.0f},  {120.0f, 30.0f, 8.0f},  {64.0f, 64.0f, 8.0f},
      {140.0f, 34.0f, 6.0f},  {88.0f, 28.0f, 6.0f},   {100.0f, 36.0f, 6.0f},
      // 中：卡片、输入框
      {320.0f, 160.0f, 12.0f}, {300.0f, 120.0f, 10.0f}, {280.0f, 200.0f, 12.0f},
      {240.0f, 48.0f, 8.0f},   {360.0f, 180.0f, 12.0f},
      // 大：面板、编辑区、滚动区
      {720.0f, 480.0f, 16.0f}, {900.0f, 600.0f, 16.0f}, {1200.0f, 700.0f, 12.0f},
  };

  Canvas canvas(kWidth, kHeight, 1.0f);
  canvas.clear(Color::rgb(0xF0, 0xF0, 0xF4));
  const Paint paint = Paint::solid(Color::rgb(0xFF, 0xFF, 0xFF));

  std::map<const char*, Bucket> buckets;
  // 预热一遍建图（真实界面里形状重复出现，缓存会命中）。
  for (int warm = 0; warm < 2; ++warm) {
    float y = 10.0f;
    for (const Item& item : items) {
      canvas.fill_rect(Rect{20.0f, y, item.width, item.height}, paint, item.radius);
      y += 24.0f;
      if (y > kHeight - 60.0f) y = 10.0f;
    }
  }

  // 正式计时：每个形状画 20 次取平均。
  constexpr int kRepeat = 20;
  for (const Item& item : items) {
    const Rect box{40.0f, 40.0f, item.width, item.height};
    const double area = static_cast<double>(item.width) * static_cast<double>(item.height);
    // 取多次最小值，排除调度噪声。
    double best = 1e9;
    for (int round = 0; round < 30; ++round) {
      const auto start = std::chrono::steady_clock::now();
      for (int index = 0; index < kRepeat; ++index) {
        canvas.fill_rect(box, paint, item.radius);
      }
      const auto end = std::chrono::steady_clock::now();
      best = std::min(best,
                      std::chrono::duration<double, std::milli>(end - start).count());
    }
    Bucket& bucket = buckets[bucket_of(area)];
    bucket.calls += kRepeat;
    bucket.total_ms += best;
    bucket.total_px += area * static_cast<double>(kRepeat);
  }

  st::log::out("{:<28}  {:>8}  {:>12}  {:>14}\n", "尺寸档", "次数", "ns/px", "占比");
  double all_ms = 0.0;
  for (const auto& [name, bucket] : buckets) all_ms += bucket.total_ms;
  for (const auto& [name, bucket] : buckets) {
    const double ns_per_px = bucket.total_px > 0.0 ? bucket.total_ms * 1e6 / bucket.total_px : 0.0;
    st::log::out("{:<28}  {:>8}  {:>12.3f}  {:>13.1f}%\n", name, bucket.calls, ns_per_px,
              all_ms > 0.0 ? bucket.total_ms / all_ms * 100.0 : 0.0);
  }
  st::log::out("\n合计 {:.3f} ms（{} 次绘制）\n", all_ms,
            static_cast<int>(buckets["小 <5k px（按钮/标签）"].calls +
                             buckets["中 5k~50k（卡片/输入框）"].calls +
                             buckets["大 >50k（面板/编辑区）"].calls));
  st::log::out("理论下界（0.19 ns/px，见 frame_floor_probe）：{:.3f} ms\n",
            [&] {
              double px = 0.0;
              for (const auto& [name, bucket] : buckets) px += bucket.total_px;
              return px * 0.19 / 1e6;
            }());
  return st::probe::main_of(spec, opts, verdict);
}
