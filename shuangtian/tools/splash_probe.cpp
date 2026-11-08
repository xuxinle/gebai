/// 启动卡的离屏渲染探针：把 `paint_splash_card` 在几个时间点各画一张 PNG。
///
/// ## 为什么是离屏而不是截图
///
/// 启动卡只活 ~320ms，而一次屏幕抓取要 ~250ms（全窗 BitBlt + DIB 回读），
/// 加上窗口可能被别的窗口挡住、DWM 合成的 acrylic 内容用 `GetDC` 抓不到
/// （抓到的是重定向表面的黑）——**截图在这件事上不可靠**。
/// 而卡本身是纯光栅输出，离屏画一张就能逐像素看，还能做回归基线。
///
/// ## 出什么图
///
/// 每张 900×600（逻辑 600×400 @1.5x，够看清圆角与抗锯齿），三组：
///
/// | 文件 | 场景 | 判据（人眼） |
/// |---|---|---|
/// | `splash-blur-t*.png` | 有磨砂（卡外透明） | 卡外应是**全透明**（PNG alpha=0），卡内半透明 |
/// | `splash-solid-t*.png` | 无磨砂（卡外填主题底色） | 卡外是主题底色，卡内近不透明 |
/// | `splash-sheet.png` | 上述两组的时间序列拼图 | 脉冲环在扩散、扫光在移动（帧间有差异） |
///
/// 另有断言：**帧间必须有像素差异**（否则动效没动）——这条是防"画了个静态图当动画"。
///
/// 用法：`splash_probe [输出目录]`（缺省 `build/splash`）。

#include <cstdio>
#include <string>
#include <vector>

#include "st/app/splash.hpp"
#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"

namespace {

constexpr int kLogicalWidth = 600;
constexpr int kLogicalHeight = 400;
constexpr float kScale = 1.5f;

/// RGBA8（非预乘；画布是预乘 `0xRRGGBBAA`，PNG 要非预乘）。
auto to_png(const st::raster::Canvas& canvas) -> st::codec::PngImage {
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  st::codec::PngImage image{};
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  const auto pixels = canvas.pixels();
  for (std::size_t index = 0; index < pixels.size(); ++index) {
    const std::uint32_t value = pixels[index];
    const std::uint32_t a = value & 0xFFU;
    const std::uint32_t b = (value >> 8U) & 0xFFU;
    const std::uint32_t g = (value >> 16U) & 0xFFU;
    const std::uint32_t r = (value >> 24U) & 0xFFU;
    // 预乘 → 非预乘（PNG 存的是直通颜色；不还原会让半透明区偏暗）。
    const auto un = [a](std::uint32_t channel) -> std::uint8_t {
      if (a == 0U) return 0;
      const auto value_f = static_cast<float>(channel) * 255.0f / static_cast<float>(a);
      return static_cast<std::uint8_t>(value_f > 255.0f ? 255.0f : value_f);
    };
    image.rgba[index * 4U + 0U] = un(r);
    image.rgba[index * 4U + 1U] = un(g);
    image.rgba[index * 4U + 2U] = un(b);
    image.rgba[index * 4U + 3U] = static_cast<std::uint8_t>(a);
  }
  return image;
}

/// 两张画布之间"有差异的像素数"（动效存活的判据）。
auto diff_pixels(const st::raster::Canvas& a, const st::raster::Canvas& b) -> int {
  const auto left = a.pixels();
  const auto right = b.pixels();
  const std::size_t count = left.size() < right.size() ? left.size() : right.size();
  int differing = 0;
  for (std::size_t index = 0; index < count; ++index) {
    if (left[index] != right[index]) ++differing;
  }
  return differing;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : "build/splash";
  (void)st::fs::create_directories(out_dir);

  // 暗色主题的取值（gallery dark 的 bg / accent），与真实启动一致。
  const st::math::Color background = st::math::Color::rgb(0x0B, 0x0B, 0x0E);
  const st::math::Color accent = st::math::Color::rgb(0x7C, 0x8C, 0xFF);

  st::app::SplashStyle style{};
  const std::vector<double> times{0.0, 0.28, 0.56, 0.84, 1.12};
  int failures = 0;

  // ── 有磨砂：卡外必须**全透明**（alpha=0），否则合成器透不出磨砂 ──
  std::vector<st::raster::Canvas> blur_frames;
  for (std::size_t index = 0; index < times.size(); ++index) {
    st::app::SplashState state{};
    state.elapsed_seconds = times[index];
    state.progress = static_cast<float>(0.25 + 0.15 * static_cast<double>(index));
    state.background = background;
    state.accent = accent;
    state.backdrop_blur = true;

    st::raster::Canvas canvas =
        st::raster::Canvas::for_logical_size(kLogicalWidth, kLogicalHeight, kScale);
    st::app::paint_splash_card(canvas, state, style);

    // 判据 ①：四角必须是全透明（有磨砂时卡外不画任何东西）。
    const int width = canvas.physical_width();
    const int height = canvas.physical_height();
    const auto pixels = canvas.pixels();
    const auto alpha_at = [&](int x, int y) -> std::uint32_t {
      return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x)] & 0xFFU;
    };
    const std::uint32_t corner_alpha =
        alpha_at(2, 2) + alpha_at(width - 3, 2) + alpha_at(2, height - 3) +
        alpha_at(width - 3, height - 3);
    if (corner_alpha != 0U) {
      std::printf("  [FAIL] 磨砂档第 %zu 帧：四角 alpha 合计 %u（应为 0）\n", index,
                  corner_alpha);
      ++failures;
    }
    // 判据 ②：卡中心必须**不是**全透明（卡真的画出来了）。
    const std::uint32_t center_alpha =
        alpha_at(width / 2, height / 2) + alpha_at(width / 2, height / 2 - 40);
    if (center_alpha == 0U) {
      std::printf("  [FAIL] 磨砂档第 %zu 帧：卡中心全透明（卡没画出来）\n", index);
      ++failures;
    }
    const st::codec::PngImage image = to_png(canvas);
    (void)st::codec::png_write_file(st::fs::join(out_dir, std::format("splash-blur-t{}.png", index)),
                                    image);
    blur_frames.push_back(std::move(canvas));
  }

  // ── 无磨砂：卡外必须是主题底色（不透明），否则会透出桌面壁纸 ──
  for (std::size_t index = 0; index < times.size(); ++index) {
    st::app::SplashState state{};
    state.elapsed_seconds = times[index];
    state.progress = 0.5f;
    state.background = background;
    state.accent = accent;
    state.backdrop_blur = false;

    st::raster::Canvas canvas =
        st::raster::Canvas::for_logical_size(kLogicalWidth, kLogicalHeight, kScale);
    st::app::paint_splash_card(canvas, state, style);

    const int width = canvas.physical_width();
    const auto pixels = canvas.pixels();
    const std::uint32_t corner = pixels[static_cast<std::size_t>(2) * static_cast<std::size_t>(width) + 2];
    if ((corner & 0xFFU) != 0xFFU) {
      std::printf("  [FAIL] 无磨砂档第 %zu 帧：角像素不透明（alpha=%u）\n", index, corner & 0xFFU);
      ++failures;
    }
    (void)st::codec::png_write_file(
        st::fs::join(out_dir, std::format("splash-solid-t{}.png", index)), to_png(canvas));
  }

  // ── 判据 ③：帧间必须有差异（否则「动效」是静态图） ──
  for (std::size_t index = 1; index < blur_frames.size(); ++index) {
    const int differing = diff_pixels(blur_frames[index - 1], blur_frames[index]);
    std::printf("  帧 %zu → %zu 差异像素 %d\n", index - 1, index, differing);
    if (differing < 200) {
      std::printf("  [FAIL] 相邻帧几乎相同（%d 像素）：动效没动\n", differing);
      ++failures;
    }
  }

  // ── 人眼用的预览：把卡**合成到模拟的磨砂背景**上，并放大卡区 ──
  //
  // 为何需要它：裸 PNG 上"卡外全透明"看不出磨砂效果（透明区域在查看器里是棋盘格）。
  // 真实观感是"卡浮在一块被模糊的桌面上"，所以这里自己造一块带模糊斑块的底
  //（近似 acrylic 后的桌面），再用预乘 alpha 把卡叠上去。**这不是渲染回归基线**，
  // 只用于「看起来像不像那么回事」的判断。
  for (std::size_t index = 0; index < blur_frames.size(); ++index) {
    st::raster::Canvas preview = st::raster::Canvas::for_logical_size(600, 400, kScale);
    const int width = preview.physical_width();
    const int height = preview.physical_height();
    {
      auto pixels = preview.pixels();
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          // 模拟**真实的暗色磨砂背景**：实测（深色主题 + acrylic + 黑底终端在后面）
          // 上屏后背景亮度只有 7~27，不是"好看的蓝紫"。第一版预览用了亮蓝色底，
          // 于是"卡在暗背景上看不见"这个缺陷在预览里完全看不到——
          // **预览背景必须取真实值**，否则预览就是另一个假阳性。
          const float fx = static_cast<float>(x) / static_cast<float>(width);
          const float fy = static_cast<float>(y) / static_cast<float>(height);
          const float wash = 0.5f + 0.5f * std::sin(fx * 5.5f + 0.7f) * std::cos(fy * 3.2f);
          const auto r = static_cast<std::uint8_t>(6.0f + 16.0f * wash);
          const auto g = static_cast<std::uint8_t>(7.0f + 18.0f * wash);
          const auto b = static_cast<std::uint8_t>(10.0f + 24.0f * wash);
          pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)] = (static_cast<std::uint32_t>(r) << 24U) |
                                                (static_cast<std::uint32_t>(g) << 16U) |
                                                (static_cast<std::uint32_t>(b) << 8U) | 0xFFU;
        }
      }
    }
    // 预乘 src-over 把卡叠上去（两边都是预乘 `0xRRGGBBAA`）。
    const auto card_pixels = blur_frames[index].pixels();
    auto base = preview.pixels();
    for (std::size_t i = 0; i < base.size() && i < card_pixels.size(); ++i) {
      const std::uint32_t s = card_pixels[i];
      const std::uint32_t sa = s & 0xFFU;
      if (sa == 0U) continue;
      const std::uint32_t inv = 255U - sa;
      const auto blend = [&](int shift) -> std::uint32_t {
        const std::uint32_t sc = (s >> shift) & 0xFFU;
        const std::uint32_t dc = (base[i] >> shift) & 0xFFU;
        return (sc + (dc * inv + 127U) / 255U) & 0xFFU;
      };
      base[i] = (blend(24) << 24U) | (blend(16) << 16U) | (blend(8) << 8U) |
                ((sa + ((base[i] & 0xFFU) * inv + 127U) / 255U) & 0xFFU);
    }
    (void)st::codec::png_write_file(
        st::fs::join(out_dir, std::format("splash-preview-t{}.png", index)), to_png(preview));
  }

  std::printf("\n产物目录：%s\n", out_dir.c_str());
  std::printf(failures == 0 ? "结论：通过（磨砂档卡外全透明 / 无磨砂档卡外不透明 / 帧间有动效）\n"
                            : "结论：%d 项失败\n",
              failures);
  return failures == 0 ? 0 : 1;
}
