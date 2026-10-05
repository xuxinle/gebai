// 边框实现等价性 / 成本对照（性能轮，修正版）。
//
// ## 为什么有修正版
//
// 第一版用 `Path::reverse()` 造环形的内圈——**它把每个段都拆成独立 MoveTo**（见 `Path::reverse`
// 的遍历方式：反向遍历时把 CubicTo 的终点写成 MoveTo、丢掉两个控制点），于是内圈根本不成形，
// 量出来"11.6% 像素不同"是**探针自身的缺陷**，不是候选方案的。
// 本版按 `src/ui/svg.cpp` 里那份**已知正确**的做法造内圈：先扁平化，再按点序倒序重发。
//
// 对照三组：
//   ① 描边（现状）：`stroke_path(圆角矩形内缩 w/2, w)`
//   ② 环形填充（候选）：外圈圆角矩形 + 内圈（倒序点）——一次 `fill_path`
//   ③ 直角对照：`add_rect` 的描边 vs 环形
//
// 输出：超差像素 / 最大通道差，以及两者的耗时。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/theme.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline constexpr int kWidth = 400;
inline constexpr int kHeight = 300;

struct Diff {
  std::size_t differing{0};
  int max_delta{0};
  int worst_x{0};
  int worst_y{0};
  std::size_t total{0};
};

auto diff(const st::raster::Canvas& left, const st::raster::Canvas& right) -> Diff {
  Diff result;
  for (int y = 0; y < left.physical_height(); ++y) {
    for (int x = 0; x < left.physical_width(); ++x) {
      const st::math::Color a = left.pixel_at(x, y);
      const st::math::Color b = right.pixel_at(x, y);
      const int delta = std::max({std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)),
                                  std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)),
                                  std::abs(static_cast<int>(a.b) - static_cast<int>(b.b)),
                                  std::abs(static_cast<int>(a.a) - static_cast<int>(b.a))});
      ++result.total;
      if (delta > 0) ++result.differing;
      if (delta > result.max_delta) {
        result.max_delta = delta;
        result.worst_x = x;
        result.worst_y = y;
      }
    }
  }
  return result;
}

/// 把一条路径按「点序倒序」重发（绕向翻转；曲线先扁平化）。
/// 与 `src/ui/svg.cpp` 的孔洞构造同一手法——`Path::reverse()` 不能用（那会拆散子路径）。
[[nodiscard]] auto append_reversed(st::raster::Path& out, const st::raster::Path& source,
                                   float tolerance = 0.2f) -> void {
  for (const auto& polyline : source.flatten(tolerance)) {
    if (polyline.points.size() < 2) continue;
    out.move_to(polyline.points.back());
    for (std::size_t index = polyline.points.size() - 1; index-- > 0;) {
      out.line_to(polyline.points[index]);
    }
    out.close();
  }
}

/// 环形边框（外圈 + 倒序内圈）。`outer` 为**外边缘**矩形，`width` 为边框厚度。
[[nodiscard]] auto make_border_ring(st::math::Rect outer, float radius, float width)
    -> st::raster::Path {
  st::raster::Path path;
  path.add_rounded_rect(outer, radius);
  st::raster::Path inner;
  const st::math::Rect box = outer.inset(st::math::Insets::all(width));
  inner.add_rounded_rect(box, radius > width ? radius - width : 0.0f);
  append_reversed(path, inner);
  return path;
}

template <class Body>
[[nodiscard]] auto best_ms(int repeat, Body&& body) -> double {
  double best = 1e9;
  for (int index = 0; index < repeat; ++index) {
    const auto start = Clock::now();
    body();
    best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
  }
  return best;
}

auto report(const std::string& name, const Diff& result) -> void {
  st::print("  {:<40} 超差 {:>6} / {} px（{:.4f}%）· 最大Δ{} @({},{})\n", name, result.differing,
            result.total,
            result.total == 0 ? 0.0
                              : 100.0 * static_cast<double>(result.differing) /
                                    static_cast<double>(result.total),
            result.max_delta, result.worst_x, result.worst_y);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const int repeat = argc > 1 ? std::atoi(argv[1]) : 300;
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::raster::Paint border = st::raster::Paint::solid(theme.colors().border);
  const st::raster::Paint white = st::raster::Paint::solid(st::math::Color::rgb(0xFF, 0xFF, 0xFF));

  struct Case {
    float card_w;
    float radius;
    float border_width;
    float dpi;
  };
  const std::vector<Case> cases{{140.0f, 12.0f, 1.0f, 1.0f}, {140.0f, 12.0f, 1.0f, 2.0f},
                                {140.0f, 12.0f, 2.0f, 1.0f}, {140.0f, 12.0f, 0.3f, 1.0f},
                                {320.0f, 4.0f, 1.0f, 1.0f},  {60.0f, 30.0f, 1.0f, 1.0f},
                                {200.0f, 40.0f, 1.5f, 3.0f}, {10.0f, 5.0f, 1.0f, 1.0f}};

  st::print("① 圆角边框：描边（现状） vs 环形填充（候选，内圈倒序点）\n");
  for (const Case& item : cases) {
    const st::math::Rect box{40.0f, 40.0f, item.card_w, 100.0f};
    const float half = item.border_width * 0.5f;
    st::raster::Canvas stroke_canvas =
        st::raster::Canvas::for_logical_size(kWidth, kHeight, item.dpi);
    stroke_canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
    stroke_canvas.fill_rect(box, white, item.radius);
    {
      st::raster::Path outline;
      outline.add_rounded_rect(box.inset(st::math::Insets::all(half)),
                               item.radius > half ? item.radius - half : 0.0f);
      stroke_canvas.stroke_path(outline, border, item.border_width);
    }
    st::raster::Canvas ring_canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, item.dpi);
    ring_canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
    ring_canvas.fill_rect(box, white, item.radius);
    ring_canvas.fill_path(make_border_ring(box, item.radius, item.border_width), border);

    report(std::format("w={:.0f} r={:.0f} bw={:.1f} dpi={:.1f}",
                       static_cast<double>(item.card_w), static_cast<double>(item.radius),
                       static_cast<double>(item.border_width), static_cast<double>(item.dpi)),
           diff(stroke_canvas, ring_canvas));
  }

  st::print("② 直角边框（对照，无曲线）\n");
  {
    const st::math::Rect box{40.0f, 40.0f, 140.0f, 100.0f};
    st::raster::Canvas stroke_canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
    stroke_canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
    stroke_canvas.fill_rect(box, white, 0.0f);
    st::raster::Path outline;
    outline.add_rect(box.inset(st::math::Insets::all(0.5f)));
    stroke_canvas.stroke_path(outline, border, 1.0f);
    st::raster::Canvas ring_canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
    ring_canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
    ring_canvas.fill_rect(box, white, 0.0f);
    st::raster::Path ring;
    ring.add_rect(box);
    st::raster::Path inner;
    inner.add_rect(box.inset(st::math::Insets::all(1.0f)));
    append_reversed(ring, inner);
    ring_canvas.fill_path(ring, border);
    report("直角 1px", diff(stroke_canvas, ring_canvas));
  }

  st::print("③ 成本（单次，最快值）\n");
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(1280, 800, 1.0f);
  canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
  const st::math::Rect box{100.0f, 100.0f, 140.0f, 48.0f};
  const st::raster::Path outline = [&] {
    st::raster::Path p;
    p.add_rounded_rect(box.inset(st::math::Insets::all(0.5f)), 11.5f);
    return p;
  }();
  const st::raster::Path ring = make_border_ring(box, 12.0f, 1.0f);
  st::print("  描边 stroke_path        {:.4} ms\n",
            best_ms(repeat, [&] { canvas.stroke_path(outline, border, 1.0f); }));
  st::print("  环形 fill_path          {:.4} ms\n",
            best_ms(repeat, [&] { canvas.fill_path(ring, border); }));
  st::print("  环形路径构造 + fill     {:.4} ms\n", best_ms(repeat, [&] {
             canvas.fill_path(make_border_ring(box, 12.0f, 1.0f), border);
           }));
  return 0;
}
