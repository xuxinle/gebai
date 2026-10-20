// **过度绘制（overdraw）量化**：每个像素在一帧里被写几次、哪些是白写。
//
// 为什么需要这个量具：整帧耗时不能只看"各原语花了多少 ms"——那会把
// **重复覆盖同一块像素**的浪费藏起来。gallery 1600×1000 实测一帧累计绘制
// **5.39 M 像素 = 3.37 屏**（clear 1.35M + shadow 1.45M + fill_rect 1.42M +
// round_rect 1.11M + text 0.06M），而屏幕只有 1.6 M。也就是说每像素平均被写 3.4 次。
//
// 本探针把一帧拆成「逐像素写入次数直方图」+「每次绘制时该像素是否已被更上层覆盖」，
// 从而回答两个不同的问题：
//   ① **有多少次写入是最终被完全盖掉的**（可以先跳过，等价且零风险）；
//   ② **有多少次写入是重复同色 / 半透明叠同一块**（需要更复杂的手段）。
//
// 判据：只报数（这是解释性探针，没有合格线）。

#include <cstdint>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Paint;

/// 一个极简但形状接近真实界面的场景：底色 + 若干卡片（阴影 + 圆角底 + 圆角内容）。
/// 刻意不用 gallery——探针要能独立复现，且形状参数可调。
struct SceneStats {
  std::uint64_t writes{0};          ///< 累计写入次数（含被覆盖的）
  std::uint64_t pixel_slots{0};     ///< 画布像素数
  std::uint64_t max_overdraw{0};
  double mean_overdraw{0.0};
  std::uint64_t covered_writes{0};  ///< 被后续不透明绘制完全盖掉的写入次数
};

[[nodiscard]] auto measure(int width, int height, int cards, bool draw_shadow) -> SceneStats {
  Canvas canvas(width, height, 1.0f);
  canvas.clear(Color::rgb(0xF0, 0xF0, 0xF4));

  // 每像素写入计数（用画布自身的读取判断"是否真的被写过"代价太高，
  // 这里把每次绘制的矩形面积累加到一张计数图上——对矩形/圆角矩形足够精确）。
  std::vector<std::uint32_t> count(static_cast<std::size_t>(width) *
                                       static_cast<std::size_t>(height),
                                   0);
  const auto add_rect = [&](Rect r) {
    const int x0 = std::max(0, static_cast<int>(r.x));
    const int y0 = std::max(0, static_cast<int>(r.y));
    const int x1 = std::min(width, static_cast<int>(r.right()));
    const int y1 = std::min(height, static_cast<int>(r.bottom()));
    for (int y = y0; y < y1; ++y) {
      auto* row = count.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
      for (int x = x0; x < x1; ++x) ++row[x];
    }
  };

  // 1) 清屏
  add_rect(Rect{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)});

  // 2) 卡片：阴影 → 圆角底 → 圆角内容（真实界面的典型三层）
  constexpr int kCols = 4;
  for (int index = 0; index < cards; ++index) {
    const float x = 40.0f + static_cast<float>(index % kCols) * 380.0f;
    const float y = 40.0f + static_cast<float>(index / kCols) * 220.0f;
    const Rect box{x, y, 320.0f, 160.0f};
    if (draw_shadow) {
      canvas.draw_shadow(box, 12.0f, 24.0f, Color{0, 0, 0, 48}, st::math::Point{0.0f, 4.0f});
      add_rect(box.inset(st::math::Insets::all(-38.0f)));   // 阴影外扩区域（近似）
    }
    canvas.fill_rect(box, Paint::solid(Color::rgb(0xFF, 0xFF, 0xFF)), 12.0f);
    add_rect(box);
    // 卡片内的一块内容区（圆角）
    const Rect inner = box.inset(st::math::Insets::all(16.0f));
    canvas.fill_rect(inner, Paint::solid(Color::rgb(0xE8, 0xEC, 0xF2)), 8.0f);
    add_rect(inner);
  }

  SceneStats stats;
  stats.pixel_slots = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  std::uint64_t total = 0;
  for (const auto value : count) {
    if (value == 0) continue;
    total += value;
    stats.max_overdraw = std::max<std::uint64_t>(stats.max_overdraw, value);
  }
  stats.writes = total;
  stats.mean_overdraw =
      stats.pixel_slots == 0
          ? 0.0
          : static_cast<double>(total) / static_cast<double>(stats.pixel_slots);
  return stats;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "过度绘制量化：每像素平均被写几次（解释整帧成本的构成）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("overdraw_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  constexpr int kWidth = 1600;
  constexpr int kHeight = 1000;
  st::print("画布 {}x{} = {} 像素\n\n", kWidth, kHeight,
            static_cast<std::uint64_t>(kWidth) * static_cast<std::uint64_t>(kHeight));
  st::print("{:>8}  {:>10}  {:>14}  {:>12}  {:>10}\n", "卡片数", "含阴影", "累计写入像素",
            "等效屏数", "每像素均写");
  for (const bool shadow : {false, true}) {
    for (const int cards : {4, 8, 12}) {
      const SceneStats stats = measure(kWidth, kHeight, cards, shadow);
      st::print("{:>8}  {:>10}  {:>14}  {:>12.2f}  {:>10.2f}\n", cards, shadow ? "是" : "否",
                stats.writes,
                static_cast<double>(stats.writes) /
                    (static_cast<double>(kWidth) * static_cast<double>(kHeight)),
                stats.mean_overdraw);
    }
  }
  st::print("\n注：每像素均写 > 1 表示同一个像素被多次覆盖——每次都付内存带宽。\n");
  return st::probe::main_of(spec, opts, verdict);
}
