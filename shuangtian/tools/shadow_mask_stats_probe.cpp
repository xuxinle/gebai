// 阴影遮罩代价解剖（性能轮）：量出 md 两层阴影的遮罩里**真实有多少像素值得混合**。
//
// 动机：`draw_shadow` 逐像素对 mask 做 `over_premul`，成本正比于「区域面积」。
// 而模糊遮罩的**尾部**（远低于可见阈值）占了很大面积——如果大面积像素的 α 小于 1%，
// 那它们的混合是纯浪费（屏幕上看不出来），一个阈值就能省掉。
//
// 输出：每层的区域面积、以及 mask 值 ≥ 阈值 的像素占比与**可见像素密度**。

#include <algorithm>
#include <cstdint>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/theme.hpp"

namespace {

/// 用「画到白底、量实际像素差」的方式反推遮罩的可见分布：
/// 比直接读私有遮罩更可靠（走的就是真实路径）。
struct Stats {
  std::size_t area{0};        ///< region 面积（像素）
  std::size_t touched{0};     ///< 与白底有可见差（Δ≥1）的像素
  std::size_t faint{0};       ///< Δ≤2 的像素（几乎不可见）
  std::size_t strong{0};      ///< Δ≥16 的像素（明显可见）
  double ink{0.0};            ///< 累计 Δ（等价于总"墨量"）
};

auto measure_shadow(const st::ui::Shadow& shadow, float blur, st::math::Color color,
                    float offset_y, bool second) -> Stats {
  constexpr int kWidth = 640;
  constexpr int kHeight = 400;
  const st::math::Rect box{200.0f, 150.0f, 140.0f, 48.0f};
  constexpr float kRadius = 12.0f;

  st::raster::Canvas base = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  base.clear(st::math::Color::rgb(0xFF, 0xFF, 0xFF));
  st::raster::Canvas drawn = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  drawn.clear(st::math::Color::rgb(0xFF, 0xFF, 0xFF));
  drawn.draw_shadow(box, kRadius, blur, color, st::math::Point{0.0f, offset_y});

  // region（含 padding）——与实现同一口径
  const float padding = blur * 2.0f + 2.0f;
  const st::math::Rect region = box.offset(0.0f, offset_y).inflate(padding);
  const st::math::IntRect area = region.round_out();

  Stats stats;
  stats.area = static_cast<std::size_t>(area.width) * static_cast<std::size_t>(area.height);
  for (int y = area.y; y < area.bottom(); ++y) {
    for (int x = area.x; x < area.right(); ++x) {
      const st::math::Color a = base.pixel_at(x, y);
      const st::math::Color b = drawn.pixel_at(x, y);
      const int delta = std::max({std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)),
                                  std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)),
                                  std::abs(static_cast<int>(a.b) - static_cast<int>(b.b))});
      if (delta >= 1) ++stats.touched;
      if (delta <= 2) ++stats.faint;
      if (delta >= 16) ++stats.strong;
      stats.ink += static_cast<double>(delta);
    }
  }
  (void)shadow;
  (void)second;
  return stats;
}

auto report(const char* name, const Stats& stats) -> void {
  const double area = static_cast<double>(stats.area);
  st::print("  {:<22} 区域 {:>7} px · 有差 {:>6.1}% · 微弱(Δ≤2) {:>6.1}% · 明显(Δ≥16) {:>6.1}% · "
            "总墨量 {:>10.0f}\n",
            name, stats.area, 100.0 * static_cast<double>(stats.touched) / area,
            100.0 * static_cast<double>(stats.faint) / area,
            100.0 * static_cast<double>(stats.strong) / area, stats.ink);
}

}  // namespace

auto main() -> int {
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::Shadow sm = st::ui::shadow_sm(theme);
  const st::ui::Shadow md = st::ui::shadow_md(theme);
  const st::ui::Shadow lg = st::ui::shadow_lg(theme);
  st::print("阴影遮罩可见性分布（140×48 卡片，radius 12，白底）：\n");
  report("sm 关键层 blur=3", measure_shadow(sm, sm.blur, sm.color, sm.offset_y, false));
  report("sm 环境层 blur=10", measure_shadow(sm, sm.blur2, sm.color2, sm.offset2_y, true));
  report("md 关键层 blur=6", measure_shadow(md, md.blur, md.color, md.offset_y, false));
  report("md 环境层 blur=22", measure_shadow(md, md.blur2, md.color2, md.offset2_y, true));
  report("lg 关键层 blur=10", measure_shadow(lg, lg.blur, lg.color, lg.offset_y, false));
  report("lg 环境层 blur=40", measure_shadow(lg, lg.blur2, lg.color2, lg.offset2_y, true));

  // 对照：仅 key 层（单层阴影）的可见量
  st::print("对照：\n");
  report("md 两层合计", measure_shadow(md, md.blur, md.color, md.offset_y, false));
  return 0;
}
