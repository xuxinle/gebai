// 阴影「遮罩构建 vs 合成」成本分离（性能轮）。
//
// 决定性问题：`shadow_mask` 有缓存（按几何参数），而基准每帧都标脏整树重绘——
// 那 14 ms 到底是**每帧重建遮罩**（光栅化 + 3 次盒式模糊），还是**只是逐像素合成**？
// 两者的优化手段完全不同（前者要改缓存策略/模糊实现，后者要改合成核）。
//
// 做法：同一张画布上对同一几何连续调用 `draw_shadow`，比较**首次**（缓存未命中）
// 与**后续**（命中）的耗时；再用"每遍换一个几何"模拟全不命中。

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
#include "st/ui/theme.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline constexpr int kWidth = 1280;
inline constexpr int kHeight = 800;

auto elapsed_ms(const Clock::time_point& start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const int repeat = argc > 1 ? std::atoi(argv[1]) : 60;
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::Shadow md = st::ui::shadow_md(theme);
  const st::math::Rect box{200.0f, 200.0f, 140.0f, 48.0f};
  constexpr float kRadius = 12.0f;

  auto make_canvas = [] {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
    canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
    return canvas;
  };

  // ① 首次（缓存未命中） vs 后续（命中）
  {
    st::raster::Canvas canvas = make_canvas();
    const auto first_start = Clock::now();
    canvas.draw_shadow(box, kRadius, md.blur2, md.color2,
                       st::math::Point{md.offset2_x, md.offset2_y});
    const double first_ms = elapsed_ms(first_start);
    double best = 1e9;
    for (int index = 0; index < repeat; ++index) {
      const auto start = Clock::now();
      canvas.draw_shadow(box, kRadius, md.blur2, md.color2,
                         st::math::Point{md.offset2_x, md.offset2_y});
      best = std::min(best, elapsed_ms(start));
    }
    st::print("① 环境层 blur=22：首次（建遮罩+模糊+合成）{:>8.3} ms · 后续（仅合成）{:>8.3} ms · "
              "建遮罩占比 {:.0f}%\n",
              first_ms, best, 100.0 * std::max(0.0, first_ms - best) / std::max(first_ms, 1e-9));
  }
  {
    st::raster::Canvas canvas = make_canvas();
    const auto first_start = Clock::now();
    canvas.draw_shadow(box, kRadius, md.blur, md.color, st::math::Point{md.offset_x, md.offset_y});
    const double first_ms = elapsed_ms(first_start);
    double best = 1e9;
    for (int index = 0; index < repeat; ++index) {
      const auto start = Clock::now();
      canvas.draw_shadow(box, kRadius, md.blur, md.color,
                         st::math::Point{md.offset_x, md.offset_y});
      best = std::min(best, elapsed_ms(start));
    }
    st::print("② 关键层 blur=6 ：首次（建遮罩+模糊+合成）{:>8.3} ms · 后续（仅合成）{:>8.3} ms · "
              "建遮罩占比 {:.0f}%\n",
              first_ms, best, 100.0 * std::max(0.0, first_ms - best) / std::max(first_ms, 1e-9));
  }

  // ③ 全不命中：每遍换一个小数偏移（0.25 量化后仍多数不同）——模拟"遮罩缓存失效"
  {
    st::raster::Canvas canvas = make_canvas();
    double best = 1e9;
    for (int index = 0; index < repeat; ++index) {
      const auto start = Clock::now();
      canvas.draw_shadow(box, kRadius, md.blur2, md.color2,
                         st::math::Point{0.0f, 6.0f + static_cast<float>(index % 16) * 0.25f});
      best = std::min(best, elapsed_ms(start));
    }
    st::print("③ 环境层 每遍换几何（全未命中，缓存上限 48）：{:>8.3} ms\n", best);
  }

  // ④ 96 张卡片形态：同一几何 → 命中 96 次（真实基准的情形）
  {
    st::raster::Canvas canvas = make_canvas();
    const auto start = Clock::now();
    for (int index = 0; index < 96; ++index) {
      const float x = 40.0f + static_cast<float>(index % 8) * 150.0f;
      const float y = 40.0f + static_cast<float>(index / 8) * 60.0f;
      canvas.draw_shadow(st::math::Rect{x, y, 140.0f, 48.0f}, kRadius, md.blur2, md.color2,
                         st::math::Point{md.offset2_x, md.offset2_y});
      canvas.draw_shadow(st::math::Rect{x, y, 140.0f, 48.0f}, kRadius, md.blur, md.color,
                         st::math::Point{md.offset_x, md.offset_y});
    }
    st::print("④ 96 张卡片两层阴影（同几何，缓存命中）：{:>8.3} ms/帧\n", elapsed_ms(start));
  }

  // ⑤ 对照：96 张卡片两层阴影 + 每遍强制换几何（全未命中）
  {
    st::raster::Canvas canvas = make_canvas();
    const auto start = Clock::now();
    for (int index = 0; index < 96; ++index) {
      const float x = 40.0f + static_cast<float>(index % 8) * 150.0f;
      const float y = 40.0f + static_cast<float>(index / 8) * 60.0f;
      canvas.draw_shadow(st::math::Rect{x, y, 140.0f, 48.0f}, kRadius, md.blur2, md.color2,
                         st::math::Point{0.0f, static_cast<float>(index % 64) * 0.25f});
      canvas.draw_shadow(st::math::Rect{x, y, 140.0f, 48.0f}, kRadius, md.blur, md.color,
                         st::math::Point{static_cast<float>(index % 64) * 0.25f, 0.0f});
    }
    st::print("⑤ 96 张卡片两层阴影（每张换几何，全未命中）：{:>8.3} ms/帧\n", elapsed_ms(start));
  }
  return 0;
}
