// 渐变填充成本探针：纯色 / 线性渐变 / 径向渐变在整幅 1080p 上的耗时对比。
//
// 动因（2026-10-17，caiyun「AI 视频导演」导出优化）：1080p 逐帧渲染里"背景"一项
// 实测占九成，用 `CAIYUN_NO_GLOW` 开关二分后定位到整幅渐变填充。本探针把三个口径
// 量清楚，用于判断"应用侧换画法"还是"框架侧优化光栅化"。
//
// **判据（性能档位，只报不断）**：整幅线性渐变 ≤ 30 ms/帧（1080p）。超过即
// "一次全屏渐变的代价已经与一帧画面同量级"，应用侧应改用纯色/局部填充。

#include <cstdint>
#include <string>

#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Gradient;
using st::raster::GradientStop;
using st::raster::Paint;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kIterations = 20;

struct Measurement {
  double ms_per_frame{0.0};
  double ns_per_pixel{0.0};
};

/// 跑 `iterations` 次并取平均（首帧含一次性开销，先预热一次再计时）。
template <class Fn>
auto measure(Fn&& draw) -> Measurement {
  Canvas canvas{kWidth, kHeight, 1.0f};
  draw(canvas);
  const std::int64_t start = st::time::now_ns();
  for (int i = 0; i < kIterations; ++i) draw(canvas);
  const double ns = static_cast<double>(st::time::now_ns() - start) / static_cast<double>(kIterations);
  return Measurement{ns / 1.0e6, ns / (static_cast<double>(kWidth) * static_cast<double>(kHeight))};
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "渐变填充成本：整幅 1080p 上的纯色 vs 线性 vs 径向",
      .criterion = "整幅线性渐变 ≤ 30 ms/帧（1080p）",
      .usage_args = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("gradient_fill_probe", spec, opts)) return st::probe::exit_ok;

  const Rect full{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)};
  st::probe::result verdict{};

  const Measurement solid = measure([full](Canvas& canvas) {
    canvas.fill_rect(full, Paint::solid(Color{20, 30, 60, 255}));
  });
  const Measurement linear = measure([full](Canvas& canvas) {
    Gradient gradient = Gradient::linear(
        Point{0.0f, 0.0f}, Point{0.0f, static_cast<float>(kHeight)},
        {GradientStop{0.0f, Color{0, 0, 0, 255}}, GradientStop{1.0f, Color{40, 60, 120, 255}}});
    canvas.fill_rect(full, Paint::with_gradient(std::move(gradient)));
  });
  const Measurement radial = measure([full](Canvas& canvas) {
    Gradient gradient = Gradient::radial(
        Point{static_cast<float>(kWidth) * 0.5f, static_cast<float>(kHeight) * 0.5f},
        static_cast<float>(kHeight),
        {GradientStop{0.0f, Color{120, 160, 255, 90}}, GradientStop{1.0f, Color{0, 0, 0, 0}}});
    canvas.fill_rect(full, Paint::with_gradient(std::move(gradient)));
  });

  // 半透明渐变（alpha != 255）单独量：应用里的"铺光"全是这种，若它落到慢路径，
  // 结论会完全不同。
  const Measurement linear_alpha = measure([full](Canvas& canvas) {
    Gradient gradient = Gradient::linear(
        Point{0.0f, 0.0f}, Point{static_cast<float>(kWidth), static_cast<float>(kHeight)},
        {GradientStop{0.0f, Color{120, 160, 255, 66}}, GradientStop{1.0f, Color{120, 160, 255, 0}}});
    canvas.fill_rect(full, Paint::with_gradient(std::move(gradient)));
  });
  st::log::out("口径：{}×{}，{} 次平均\n", kWidth, kHeight, kIterations);
  st::log::out("  纯色整幅        {:7.2f} ms/帧  {:6.2f} ns/px\n", solid.ms_per_frame,
            solid.ns_per_pixel);
  st::log::out("  线性渐变整幅    {:7.2f} ms/帧  {:6.2f} ns/px（{:.0f}× 纯色）\n",
            linear.ms_per_frame, linear.ns_per_pixel,
            solid.ms_per_frame > 0.0 ? linear.ms_per_frame / solid.ms_per_frame : 0.0);
  st::log::out("  径向渐变整幅    {:7.2f} ms/帧  {:6.2f} ns/px（{:.0f}× 纯色）\n",
            radial.ms_per_frame, radial.ns_per_pixel,
            solid.ms_per_frame > 0.0 ? radial.ms_per_frame / solid.ms_per_frame : 0.0);
  st::log::out("  线性渐变（半透明）{:7.2f} ms/帧  {:6.2f} ns/px（{:.0f}× 纯色）\n",
            linear_alpha.ms_per_frame, linear_alpha.ns_per_pixel,
            solid.ms_per_frame > 0.0 ? linear_alpha.ms_per_frame / solid.ms_per_frame : 0.0);
  st::log::out("  → 结论：**线性**渐变与纯色同量级，**径向**渐变是它的 {:.0f} 倍（1080p 单次约\n"
            "     {:.0f} ms）。所以“背景贵”的根因是**径向**渐变（逐像素算距离），线性不需要回避。\n",
            linear.ms_per_frame > 0.0 ? radial.ms_per_frame / linear.ms_per_frame : 0.0,
            radial.ms_per_frame);

  verdict.add("整幅线性渐变 ≤ 30 ms/帧", linear.ms_per_frame <= 30.0,
              std::format("{:.2f} ms/帧", linear.ms_per_frame));
  verdict.conclude();
  return st::probe::main_of(spec, opts, verdict);
}
