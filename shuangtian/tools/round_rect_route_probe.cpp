// **圆角矩形：三带拆分 vs 几何贴图缓存**——决定该不该改成走缓存。
//
// 背景（本轮实测）：
//   - gallery 1600×1000 一帧 `round_rect` **33 次 / 1.38~1.93 ms / 占整帧 37%**
//   - 而整帧理论下界（纯多层混合）是 0.19 ns/px，`round_rect` 还在 1.24~1.74 ns/px
//   - `fill_rect(radius>0)` 目前走**三带拆分 + 每帧真光栅化**，且**完全不用几何缓存**
//     （缓存只在 `fill_path` 里——这是本轮才发现的，见 BACKLOG）
//
// 两条候选路线：
//   A) 现状：三带拆分（上/下带各一次完整圆角路径光栅化 + 中带一次直角路径）
//   B) 走几何贴图缓存：首帧整块光栅化一次建图，之后每帧只贴图
//
// 判据：只报数（看 B 在**稳态**（命中）下是否真比 A 快，以及快多少）。

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
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
constexpr int kRounds = 50;

[[nodiscard]] auto best_ms(const std::function<void()>& body) -> double {
  double best = 1e9;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto end = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::milli>(end - start).count());
  }
  return best;
}

struct Shape {
  const char* name;
  float width;
  float height;
  float radius;
};

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "圆角矩形：三带拆分（现状）vs 几何贴图缓存（候选）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("round_rect_route_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  const Shape shapes[] = {
      {"按钮 96x32 r6", 96.0f, 32.0f, 6.0f},
      {"卡片 320x160 r12", 320.0f, 160.0f, 12.0f},
      {"面板 720x480 r16", 720.0f, 480.0f, 16.0f},
  };
  const Paint paint = Paint::solid(Color{0x2F, 0x6F, 0xE0, 255});

  st::log::out("画布 {}x{}，每项 {} 轮取最小\n\n", kWidth, kHeight, kRounds);
  st::log::out("{:<20}  {:>12}  {:>12}  {:>10}  {:>14}\n", "形状", "A 三带 ms", "B 贴图 ms",
            "B/A", "A 的 ns/px");
  for (const Shape& shape : shapes) {
    const Rect box{100.0f, 100.0f, shape.width, shape.height};
    const double area = static_cast<double>(shape.width) * static_cast<double>(shape.height);

    // A) 现状：`fill_rect(radius>0)` = 三带拆分，每帧真光栅化。
    Canvas canvas_a(kWidth, kHeight, 1.0f);
    canvas_a.clear(Color::rgb(0xF0, 0xF0, 0xF4));
    const double a_ms = best_ms([&] { canvas_a.fill_rect(box, paint, shape.radius); });

    // B) 候选：等价路径 = `fill_path(圆角路径)`，它**走几何贴图缓存**。
    //    预热一次让缓存建好后，后续都是命中。
    st::math::Path path;
    path.add_rounded_rect(box, shape.radius);
    Canvas canvas_b(kWidth, kHeight, 1.0f);
    canvas_b.clear(Color::rgb(0xF0, 0xF0, 0xF4));
    canvas_b.fill_path(path, paint);   // 预热：建贴图
    const double b_ms = best_ms([&] { canvas_b.fill_path(path, paint); });

    st::log::out("{:<20}  {:>12.3f}  {:>12.3f}  {:>10.2f}  {:>14.3f}\n", shape.name, a_ms, b_ms,
              b_ms > 0.0 ? a_ms / b_ms : 0.0, a_ms * 1e6 / area);
  }
  st::log::out("\n注：B 会先预热一次（建贴图），所以 B 报的是**稳态命中**成本。\n");
  st::log::out("    两者逐像素等价已由 tests/raster_fill_test.cpp 的\n");
  st::log::out("    `fill_round_rect_split_matches_path_rasterization`（零容差）钉住。\n");
  return st::probe::main_of(spec, opts, verdict);
}
