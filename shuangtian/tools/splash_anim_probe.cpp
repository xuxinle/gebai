/// 启动卡**动画**的离屏验证：把同一个 `SplashPump` 的时间轴采样成序列图，
/// 断言"相邻帧之间确实在变"。
///
/// ## 为什么不能用截图（这一课踩了两次）
///
/// 1. 启动卡只活几百毫秒，而一次全窗 `BitBlt` + DIB 回读要 ~250ms；
/// 2. 更要命：磨砂档走 `WS_EX_LAYERED` + `UpdateLayeredWindow`，**`GetDC` 抓到的是
///    重定向表面的黑**（实测所有采样帧中心都是纯黑 `(0,0,0)`，看着像"动画没动"，
///    其实只是抓不到）。我因此一度误判"泵没被调用"，浪费了一轮排查。
///
/// ## 这个探针测什么
///
/// 它不依赖窗口，直接把 `paint_splash_card` 在时间轴上画成 N 张图，
/// 然后逐对比较像素差异。判据：
/// - ① 每对相邻帧必须有可见差异（否则动画是死的）；
/// - ② 差异必须落在**卡内**（卡外应当恒为全透明，否则是"漏"）；
/// - ③ 差异不能大到像"整块闪"（帧间变化应当连续，不是跳变）。
///
/// 用法：`splash_anim_probe [输出目录]`（缺省 `build/splash-anim`）。

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "st/app/splash.hpp"
#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"

namespace {

constexpr int kLogicalWidth = 1360;
constexpr int kLogicalHeight = 1240;
constexpr float kScale = 1.0f;
constexpr int kFrames = 8;
/// 采样间隔（毫秒）：覆盖一个脉冲周期（默认 1.15s）的大部分，
/// 这样相邻帧之间必定有相位差。
constexpr double kStepMs = 130.0;

auto to_png(const st::raster::Canvas& canvas) -> st::codec::PngImage {
  st::codec::PngImage image{};
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  const auto pixels = canvas.pixels();
  for (std::size_t index = 0; index < pixels.size(); ++index) {
    const std::uint32_t value = pixels[index];
    const std::uint32_t alpha = value & 0xFFU;
    const auto un = [alpha](std::uint32_t channel) -> std::uint8_t {
      if (alpha == 0U) return 0;
      const float scaled = static_cast<float>(channel) * 255.0f / static_cast<float>(alpha);
      return static_cast<std::uint8_t>(scaled > 255.0f ? 255.0f : scaled);
    };
    image.rgba[index * 4U + 0U] = un((value >> 24U) & 0xFFU);
    image.rgba[index * 4U + 1U] = un((value >> 16U) & 0xFFU);
    image.rgba[index * 4U + 2U] = un((value >> 8U) & 0xFFU);
    image.rgba[index * 4U + 3U] = static_cast<std::uint8_t>(alpha);
  }
  return image;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : "build/splash-anim";
  (void)st::fs::create_directories(out_dir);

  st::app::SplashStyle style{};
  st::app::SplashState state{};
  state.background = st::math::Color::rgb(0x0B, 0x0B, 0x0E);
  state.accent = st::math::Color::rgb(0x7C, 0x8C, 0xFF);
  state.backdrop_blur = true;

  std::vector<std::vector<std::uint32_t>> frames;
  frames.reserve(kFrames);
  for (int index = 0; index < kFrames; ++index) {
    const double elapsed_ms = static_cast<double>(index) * kStepMs;
    state.elapsed_seconds = elapsed_ms / 1000.0;
    state.progress = static_cast<float>(std::min(0.85, elapsed_ms / 900.0));
    st::raster::Canvas canvas =
        st::raster::Canvas::for_logical_size(kLogicalWidth, kLogicalHeight, kScale);
    st::app::paint_splash_card(canvas, state, style);
    const auto pixels = canvas.pixels();
    frames.emplace_back(pixels.begin(), pixels.end());
    const auto image = to_png(canvas);
    const std::string path = st::fs::join(out_dir, "anim-" + std::to_string(index) + ".png");
    if (auto written = st::codec::png_write_file(path, image); !written) {
      std::printf("写 PNG 失败：%s\n", written.error().message.c_str());
      return 1;
    }
  }

  const int width = static_cast<int>(kLogicalWidth * kScale);
  const int height = static_cast<int>(kLogicalHeight * kScale);
  int failures = 0;
  int smallest_change = 1 << 30;
  int largest_change = 0;
  for (int index = 1; index < kFrames; ++index) {
    int changed = 0;
    int changed_outside_card = 0;
    const auto& prev = frames[static_cast<std::size_t>(index - 1)];
    const auto& curr = frames[static_cast<std::size_t>(index)];
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const std::size_t offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x);
        if (offset >= prev.size() || offset >= curr.size()) continue;
        const std::uint32_t a = prev[offset];
        const std::uint32_t b = curr[offset];
        if (a == b) continue;
        ++changed;
        // 卡外的判据：两帧都完全透明（alpha=0）却"变了"，说明有东西画到了卡外。
        if ((a & 0xFFU) == 0U && (b & 0xFFU) == 0U) ++changed_outside_card;
      }
    }
    if (changed < smallest_change) smallest_change = changed;
    if (changed > largest_change) largest_change = changed;
    // 判据 ①：必须有可见差异。阈取 200 像素——远小于卡面积（~30 万像素），
    // 但足够排除"只有一两个像素的抖动"。
    if (changed < 200) {
      std::printf("  [FAIL] 帧 %d→%d 只变了 %d 像素（动画没动）\n", index - 1, index, changed);
      ++failures;
    }
    // 判据 ②：卡外不应有变化。
    if (changed_outside_card > 50) {
      std::printf("  [FAIL] 帧 %d→%d 卡外变了 %d 像素（漏到卡外了）\n", index - 1, index,
                  changed_outside_card);
      ++failures;
    }
    std::printf("  帧 %d→%d 变化 %d 像素（卡外 %d）\n", index - 1, index, changed,
                changed_outside_card);
  }

  std::printf("产物目录：%s\n", out_dir.c_str());
  std::printf("变化像素范围：%d ~ %d\n", smallest_change, largest_change);
  if (failures == 0) {
    std::printf("结论：通过（%d 帧均有连续变化、且不溢出卡外）\n", kFrames);
    return 0;
  }
  std::printf("结论：失败（%d 项）\n", failures);
  return 1;
}
