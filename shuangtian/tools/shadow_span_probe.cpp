// 阴影合成区域的「行内跨度」统计（性能轮）：`draw_shadow` 的复合循环按 region 全矩形跑，
// 而模糊遮罩里大量像素是 0（角落尤其）。本探针量出**每行非零跨度**的总面积，
// 判断「按行收紧跨度」能省多少像素。
//
// 输出：region 面积 / 非零像素数 / 行跨度面积（含零间隙）/ 可省比例。

#include <algorithm>
#include <cstdint>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/theme.hpp"

namespace {

struct SpanStats {
  std::size_t region_area{0};
  std::size_t nonzero{0};
  std::size_t span_area{0};     ///< 按「行内首非零..末非零」算的面积
  std::size_t empty_rows{0};
  std::size_t rows{0};
};

auto stats(const st::ui::Shadow& shadow, float blur, st::math::Color color, float offset_y)
    -> SpanStats {
  constexpr int kWidth = 800;
  constexpr int kHeight = 600;
  const st::math::Rect box{300.0f, 250.0f, 140.0f, 48.0f};
  constexpr float kRadius = 12.0f;

  st::raster::Canvas base = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  base.clear(st::math::Color::rgb(0xFF, 0xFF, 0xFF));
  st::raster::Canvas drawn = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  drawn.clear(st::math::Color::rgb(0xFF, 0xFF, 0xFF));
  drawn.draw_shadow(box, kRadius, blur, color, st::math::Point{0.0f, offset_y});

  const float padding = blur * 2.0f + 2.0f;
  const st::math::IntRect area = box.offset(0.0f, offset_y).inflate(padding).round_out();
  SpanStats out;
  out.region_area = static_cast<std::size_t>(area.width) * static_cast<std::size_t>(area.height);
  for (int y = area.y; y < area.bottom(); ++y) {
    int first = -1;
    int last = -1;
    for (int x = area.x; x < area.right(); ++x) {
      const st::math::Color a = base.pixel_at(x, y);
      const st::math::Color b = drawn.pixel_at(x, y);
      if (a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a) continue;
      ++out.nonzero;
      if (first < 0) first = x;
      last = x;
    }
    ++out.rows;
    if (first < 0) {
      ++out.empty_rows;
      continue;
    }
    out.span_area += static_cast<std::size_t>(last - first + 1);
  }
  (void)shadow;
  return out;
}

auto report(const char* name, const SpanStats& s) -> void {
  const double region = static_cast<double>(s.region_area);
  const double span = static_cast<double>(s.span_area);
  st::print("  {:<20} 区域 {:>7} px · 非零 {:>6.1}% · 行跨度面积 {:>6.1}% · 空行 {:>5.1}% · "
            "跨度/非零 {:.2f}\n",
            name, s.region_area, 100.0 * static_cast<double>(s.nonzero) / region,
            100.0 * span / region, 100.0 * static_cast<double>(s.empty_rows) /
                                        static_cast<double>(s.rows),
            static_cast<double>(s.span_area) / std::max(1.0, static_cast<double>(s.nonzero)));
}

}  // namespace

auto main() -> int {
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::Shadow sm = st::ui::shadow_sm(theme);
  const st::ui::Shadow md = st::ui::shadow_md(theme);
  const st::ui::Shadow lg = st::ui::shadow_lg(theme);
  st::print("阴影 region 内的空白（决定「按行收紧」的收益）：\n");
  report("sm 关键层 blur=3", stats(sm, sm.blur, sm.color, sm.offset_y));
  report("sm 环境层 blur=10", stats(sm, sm.blur2, sm.color2, sm.offset2_y));
  report("md 关键层 blur=6", stats(md, md.blur, md.color, md.offset_y));
  report("md 环境层 blur=22", stats(md, md.blur2, md.color2, md.offset2_y));
  report("lg 关键层 blur=10", stats(lg, lg.blur, lg.color, lg.offset_y));
  report("lg 环境层 blur=40", stats(lg, lg.blur2, lg.color2, lg.offset2_y));
  return 0;
}
