// 生成镂月示例的内嵌样例 PNG（一小张可辨认的测试图：色块 + 对角斜线 + 半透明区）。
//
// 为什么要真图而不是"运行时程序化画一张"：设计文档 §3.3 要求 louyue 的
// 「打开 PNG」走**真实解码路径**（`png_decode`）。内嵌一张真编码出来的图，
// 才能覆盖"解码 → 上图 → 再导出"的往返。
//
// 用法： make_sample_png <输出路径>

#include <cstdio>
#include <string>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"

namespace {

using st::math::Color;
using st::math::Rect;

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out = argc > 1 ? argv[1] : "sample.png";
  constexpr int kWidth = 320;
  constexpr int kHeight = 200;

  st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
  canvas.clear(Color{32, 34, 44, 255});

  // 四象限色块（辨认方位用）+ 中间一条对角斜线（辨认插值/缩放用）。
  canvas.fill_rect(Rect{0.0f, 0.0f, 160.0f, 100.0f}, st::raster::Paint::solid(Color{232, 88, 88, 255}));
  canvas.fill_rect(Rect{160.0f, 0.0f, 160.0f, 100.0f}, st::raster::Paint::solid(Color{88, 200, 120, 255}));
  canvas.fill_rect(Rect{0.0f, 100.0f, 160.0f, 100.0f}, st::raster::Paint::solid(Color{88, 132, 232, 255}));
  canvas.fill_rect(Rect{160.0f, 100.0f, 160.0f, 100.0f}, st::raster::Paint::solid(Color{240, 196, 96, 255}));

  // 半透明方块：验证 alpha 通道随解码保真（导出时能看出叠色）。
  canvas.fill_rect(Rect{120.0f, 70.0f, 80.0f, 60.0f},
                   st::raster::Paint::solid(Color{255, 255, 255, 128}));

  st::codec::PngImage image;
  image.width = kWidth;
  image.height = kHeight;
  image.rgba.resize(static_cast<std::size_t>(kWidth) * kHeight * 4U);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      const std::size_t index = (static_cast<std::size_t>(y) * kWidth + x) * 4U;
      image.rgba[index + 0] = pixel.r;
      image.rgba[index + 1] = pixel.g;
      image.rgba[index + 2] = pixel.b;
      image.rgba[index + 3] = pixel.a;
    }
  }

  const auto written = st::codec::png_write_file(out, image);
  if (!written.has_value()) {
    st::print("写盘失败: {}（{}）\n", out, written.error().message);
    return 1;
  }
  st::print("已写出 {}（{}×{}）\n", out, kWidth, kHeight);
  return 0;
}
