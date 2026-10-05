// 描边路径成本分解（性能轮）：把 `stroke_path` 拆成「扁平化 / 扩展成轮廓 / 扫描线光栅化」三段，
// 判断那 50µs/次到底花在哪一步。
//
// 直接 include 内部头 `src/raster/rasterize_internal.hpp`（诊断探针，不进框架 API）。

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

#include "rasterize_internal.hpp"

#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline constexpr int kWidth = 1280;
inline constexpr int kHeight = 800;

auto elapsed_ms(const Clock::time_point& start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

template <class Body>
[[nodiscard]] auto measure(int repeat, Body&& body) -> double {
  double best = 1e9;
  for (int index = 0; index < repeat; ++index) {
    const auto start = Clock::now();
    body();
    best = std::min(best, elapsed_ms(start));
  }
  return best;
}

auto count_points(const std::vector<st::raster::Polyline>& lines) -> std::size_t {
  std::size_t total = 0;
  for (const auto& line : lines) total += line.points.size();
  return total;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const int repeat = argc > 1 ? std::atoi(argv[1]) : 200;
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));

  const st::math::Rect box{100.0f, 100.0f, 140.0f, 48.0f};
  constexpr float kRadius = 12.0f;
  const st::raster::Paint paint = st::raster::Paint::solid(st::math::Color::rgb(0xD0, 0xD5, 0xDD));

  // 圆角矩形描边路径（卡片边框的形态）
  st::raster::Path outline;
  outline.add_rounded_rect(box.inset(st::math::Insets::all(0.5f)), kRadius - 0.5f);

  const auto flat = outline.flatten(0.25f);
  const st::raster::Path expanded = st::raster::detail::stroke_to_path(outline, 1.0f, 0.25f);
  const auto expanded_flat = expanded.flatten(0.25f);

  st::print("源路径：{} 命令 · 扁平化 {} 折线 / {} 点\n", outline.commands().size(), flat.size(),
            count_points(flat));
  st::print("扩展轮廓：{} 命令 · 扁平化 {} 折线 / {} 点\n", expanded.commands().size(),
            expanded_flat.size(), count_points(expanded_flat));

  struct Row {
    std::string name;
    double ms{0.0};
  };
  std::vector<Row> rows;
  rows.push_back({"源路径 flatten(0.25)",
                  measure(repeat, [&] { (void)outline.flatten(0.25f); })});
  rows.push_back({"stroke_to_path",
                  measure(repeat, [&] { (void)st::raster::detail::stroke_to_path(outline, 1.0f, 0.25f); })});
  rows.push_back({"fill_path_aa(已扩展轮廓)",
                  measure(repeat, [&] {
                    st::raster::detail::fill_path_aa(canvas, expanded, paint,
                                                     st::raster::DrawOptions{});
                  })});
  rows.push_back({"stroke_path(端到端)", measure(repeat, [&] {
                    canvas.stroke_path(outline, paint, 1.0f);
                  })});
  rows.push_back({"fill_path(环形：外圈+反向内圈)", measure(repeat, [&] {
                    st::raster::Path ring;
                    ring.add_rounded_rect(box, kRadius);
                    st::raster::Path inner;
                    inner.add_rounded_rect(box.inset(st::math::Insets::all(1.0f)), kRadius - 1.0f);
                    inner.reverse();
                    ring.add_path(inner);
                    canvas.fill_path(ring, paint);
                  })});
  rows.push_back({"fill_rect(圆角底)", measure(repeat, [&] {
                    canvas.fill_rect(box, paint, kRadius);
                  })});
  for (const Row& row : rows) {
    st::print("  {:<34} {:>9.4} ms · {:>9.1} µs\n", row.name, row.ms, row.ms * 1000.0);
  }
  return 0;
}
