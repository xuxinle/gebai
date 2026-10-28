// **整帧耗时的下界实验**：把「每像素写一次」与「真实界面的多层覆盖」对比。
//
// 目的：给优化定一个**可信的目标**，而不是继续猜某个原语该多快。
//
// 做法：同一块画布、同样的总像素量，用两种方式写满：
//   ① **单层**：一次 `fill_rect` 覆盖整屏（每像素写 1 次）；
//   ② **多层**：分 N 次覆盖同一块区域（每像素写 N 次，模拟真实界面的
//      底色 → 卡片 → 内容 → 高光 这种层层叠加）。
//
// 两者差值就是「overdraw 的真实代价」。若 ② 明显贵于 ①×N，说明问题在
// 每像素算术；若 ② ≈ ①×N，说明纯带宽受限，唯一出路是**少写**。
//
// 判据：只报数。

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>

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
constexpr int kRounds = 40;

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

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "整帧下界：单层写 vs 多层覆盖（量化 overdraw 的真实代价）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("frame_floor_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  const double screen_px = static_cast<double>(kWidth) * static_cast<double>(kHeight);
  st::log::out("画布 {}x{} = {:.0f} 像素，每项 {} 轮取最小\n\n", kWidth, kHeight, screen_px,
            kRounds);

  const Rect full{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)};
  const Paint white = Paint::solid(Color::rgb(0xFF, 0xFF, 0xFF));

  // ① 单层：clear + 一次整屏不透明填充
  double single_ms = 0.0;
  {
    Canvas canvas(kWidth, kHeight, 1.0f);
    single_ms = best_ms([&] {
      canvas.clear(Color::rgb(0xF0, 0xF0, 0xF4));
      canvas.fill_rect(full, white);
    });
  }
  st::log::out("① 单层（clear + 1 次整屏填充）      {:>7.3f} ms  → {:.3f} ns/px\n", single_ms,
            single_ms * 1e6 / (screen_px * 2.0));

  // ② N 层：同一块区域反复覆盖（每层都真的走一遍混合）
  for (const int layers : {2, 4, 8}) {
    Canvas canvas(kWidth, kHeight, 1.0f);
    const double ms = best_ms([&] {
      canvas.clear(Color::rgb(0xF0, 0xF0, 0xF4));
      for (int index = 0; index < layers; ++index) {
        // 用带透明度的源，确保每次都真做混合（不被“同色不透明”快路径跳过）
        canvas.fill_rect(full, Paint::solid(Color{0xFF, 0xFF, 0xFF, 200}),
                         0.0f);
      }
    });
    st::log::out("② {} 层覆盖率叠（画 {}.0 屏）          {:>7.3f} ms  → {:.3f} ns/px\n", layers,
              layers + 1, ms, ms * 1e6 / (screen_px * static_cast<double>(layers + 1)));
  }

  st::log::out("\n参考：gallery 1600x1000 实测整帧 p50 ≈ 4.3 ms，累计绘制 3.37 屏。\n");
  st::log::out("      即它的“每屏成本”约 {:.2f} ms/屏。\n",
            4.3 / 3.37);
  st::log::out("      若已接近 ② 的线性区间 → 瓶颈是纯带宽，唯一出路是少写（减 overdraw）。\n");
  return st::probe::main_of(spec, opts, verdict);
}
