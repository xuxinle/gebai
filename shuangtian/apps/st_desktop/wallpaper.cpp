/// `Wallpaper` 与 `ImageBitmap` 的实现（见 wallpaper.hpp）。

#include "wallpaper.hpp"

#include <algorithm>
#include <utility>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/raster/paint.hpp"

namespace stdd {

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::DrawOptions;

[[nodiscard]] auto canvas_from_rgba(const st::codec::PngImage& image) -> Canvas {
  const int width = static_cast<int>(image.width);
  const int height = static_cast<int>(image.height);
  Canvas canvas{width, height, 1.0f};
  const std::size_t expected = static_cast<std::size_t>(width) *
                               static_cast<std::size_t>(height) * 4U;
  if (image.rgba.size() != expected) return canvas;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t at =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) * 4U;
      canvas.set_pixel(x, y, Color{image.rgba[at], image.rgba[at + 1], image.rgba[at + 2],
                                   image.rgba[at + 3]});
    }
  }
  return canvas;
}

}  // namespace

auto ImageBitmap::from_png(std::span<const std::uint8_t> bytes) -> std::shared_ptr<ImageBitmap> {
  auto bitmap = std::make_shared<ImageBitmap>();
  if (bytes.empty()) return bitmap;   // 未声明嵌入：空位 → 调用方走降级
  const auto decoded = st::codec::png_decode(bytes);
  if (!decoded) {
    st::log::err("壁纸解码失败：{}\n", decoded.error().to_string());
    return bitmap;
  }
  if (decoded->width == 0 || decoded->height == 0) return bitmap;
  bitmap->canvas = canvas_from_rgba(*decoded);
  bitmap->valid = true;
  return bitmap;
}

auto ImageBitmap::from_png(std::span<const char> bytes) -> std::shared_ptr<ImageBitmap> {
  // `b::embed` 的视图元素类型是 `char`，解码器要 `uint8_t`：拷一份转换而不是
  // `reinterpret_cast`（lint L6；char→uint8_t 的别名转换在有符号 char 平台上语义不干净）。
  // 壁纸只在启动时解码一次，几 MB 的拷贝可以忽略（与 apps/louyue 同一写法）。
  st::codec::Bytes raw;
  raw.resize(bytes.size());
  std::transform(bytes.begin(), bytes.end(), raw.begin(), [](char byte) {
    return static_cast<std::uint8_t>(static_cast<unsigned char>(byte));
  });
  return from_png(std::span<const std::uint8_t>(raw.data(), raw.size()));
}

auto ImageBitmap::from_png_file(const std::string& path) -> std::shared_ptr<ImageBitmap> {
  const auto bytes = st::fs::read_bytes(path);
  if (!bytes) {
    st::log::err("壁纸文件读不到：{}（{}）\n", path, bytes.error().to_string());
    return std::make_shared<ImageBitmap>();
  }
  return from_png(*bytes);
}

Wallpaper::Wallpaper(std::shared_ptr<const ImageBitmap> bitmap) : bitmap_(std::move(bitmap)) {
  // 桌面是装饰层：让点击穿透到图标/Dock（见头文件说明）。
  set_intercepts_input(false);
}

void Wallpaper::set_bitmap(std::shared_ptr<const ImageBitmap> bitmap) {
  bitmap_ = std::move(bitmap);
  mark_dirty();
}

void Wallpaper::set_missing_hint_visible(bool visible) {
  missing_hint_ = visible;
  mark_dirty();
}

void Wallpaper::paint_content(const st::ui::RenderContext&, st::raster::Surface& canvas) const {
  const Rect box = bounds();
  if (box.is_empty()) return;
  if (bitmap_ == nullptr || !bitmap_->valid) {
    // 缺图：保持 `Style::background` 的纯色底（本元素不额外画）。桌面不该因为
    // 一张壁纸缺失就白屏——那最容易被当成"应用起不来"。
    if (missing_hint_) {
      // 只在显式要求时画一条边（调试/自检用），默认不画。
      canvas.fill_rect(Rect{box.x, box.y, box.width, 2.0f},
                       st::raster::Paint::solid(Color{0x8A, 0x30, 0x30, 0xFF}));
    }
    return;
  }

  const float source_w = static_cast<float>(bitmap_->canvas.physical_width());
  const float source_h = static_cast<float>(bitmap_->canvas.physical_height());
  if (source_w <= 0.0f || source_h <= 0.0f) return;

  // **cover**：等比放大到至少铺满，多出来的居中裁掉。
  //
  // 为什么不是 stretch：壁纸是 16:9，而窗口可能是任意比例（用户会把它切成方形
  // 窗口调试）。直接拉伸会把纹理糊成方向性条纹——一眼看出是拉伸。
  const float scale = std::max(box.width / source_w, box.height / source_h);
  const float draw_w = source_w * scale;
  const float draw_h = source_h * scale;
  const Rect destination{box.x + (box.width - draw_w) * 0.5f,
                         box.y + (box.height - draw_h) * 0.5f, draw_w, draw_h};

  // 裁到自己的 bounds：`draw_canvas` 内部按 `clip_rect()` 求交，所以裁切本身免费。
  // 不裁的话 cover 溢出的部分会画到兄弟元素区域上（Dock 背后的像素就是它们）。
  canvas.push_clip_rect(box);
  canvas.draw_canvas(bitmap_->canvas, destination, DrawOptions{.opacity = 1.0f});
  canvas.pop_clip();
}

}  // namespace stdd
