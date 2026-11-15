/// `AppIcon` 与 `IconBitmap` 的实现（见 app_icon_element.hpp）。

#include "app_icon_element.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "st/codec/png.hpp"   // 仅用 Bytes 与解码接口（见 from_png(char)）
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/raster/paint.hpp"

namespace stdd {

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::DrawOptions;
using st::raster::Paint;

/// 非预乘 RGBA8 → 画布。
///
/// 逐像素写而不是整块 memcpy：`Canvas` 的缓冲是**预乘** 0xRRGGBBAA，
/// 直接拷非预乘数据会让半透明边缘发白（图标圆角外那圈）。图标尺寸只有 256²，
/// 一次逐像素的代价可以忽略。
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

auto IconBitmap::from_png(std::span<const std::uint8_t> bytes) -> std::shared_ptr<IconBitmap> {
  auto bitmap = std::make_shared<IconBitmap>();
  if (bytes.empty()) return bitmap;   // 未声明嵌入的图标：空位 → 占位绘制
  const auto decoded = st::codec::png_decode(bytes);
  if (!decoded) {
    // 如实记一条：图标坏了是"看得出来"的问题（占位字母），日志只是归因用。
    st::log::err("图标解码失败：{}\n", decoded.error().to_string());
    return bitmap;
  }
  if (decoded->width == 0 || decoded->height == 0) return bitmap;
  bitmap->canvas = canvas_from_rgba(*decoded);
  bitmap->valid = true;
  return bitmap;
}

auto IconBitmap::from_png(std::span<const char> bytes) -> std::shared_ptr<IconBitmap> {
  // `b::embed` 给的视图元素类型是 `char`，而解码器要 `uint8_t`。这里**拷一份转换**
  // 而不是 `reinterpret_cast`（lint L6）：char→uint8_t 的别名转换在“char 有符号”
  // 的平台上语义上就不干净，且图标只有几 KB，拷贝开销可忽略
  //（与 `apps/louyue/main.cpp` 的同一口子同一写法）。
  st::codec::Bytes raw;
  raw.resize(bytes.size());
  std::transform(bytes.begin(), bytes.end(), raw.begin(),
                 [](char byte) { return static_cast<std::uint8_t>(static_cast<unsigned char>(byte)); });
  return from_png(std::span<const std::uint8_t>(raw.data(), raw.size()));
}

auto IconBitmap::from_png_file(const std::string& path) -> std::shared_ptr<IconBitmap> {
  const auto bytes = st::fs::read_bytes(path);
  if (!bytes) {
    st::log::err("图标文件读不到：{}（{}）\n", path, bytes.error().to_string());
    return std::make_shared<IconBitmap>();
  }
  return from_png(*bytes);
}

AppIcon::AppIcon(std::shared_ptr<const IconBitmap> bitmap, std::string fallback_glyph,
                 float size)
    : bitmap_(std::move(bitmap)), glyph_(std::move(fallback_glyph)), size_(size) {
  // 图标是**装饰性**元素：点击由叠在它上面的透明按钮接管（macOS 桌面图标语义）。
  set_intercepts_input(false);
}

void AppIcon::set_size(float size) {
  if (size <= 0.0f || size == size_) return;
  size_ = size;
  mark_layout_dirty();
}

void AppIcon::set_dimmed(bool dimmed) {
  if (dimmed_ == dimmed) return;
  dimmed_ = dimmed;
  mark_dirty();
}

void AppIcon::set_rounding(float ratio) {
  rounding_ = ratio < 0.0f ? 0.0f : (ratio > 0.5f ? 0.5f : ratio);
  mark_dirty();
}

void AppIcon::measure(const st::ui::RenderContext&, const st::ui::Constraints&) {
  // 正方形固定边长：图标不该被父容器的剩余空间拉变形（桌面格子里它是定尺的）。
  measured_ = st::math::Size{size_, size_};
}

void AppIcon::paint_content(const st::ui::RenderContext&, st::raster::Surface& canvas) const {
  const Rect box = bounds();
  if (box.is_empty()) return;
  const float side = box.width < box.height ? box.width : box.height;
  const float radius = side * rounding_;
  // 画在 box 左上角的正方形区（不足正方形的部分留空——不拉伸）。
  const Rect square{box.x, box.y, side, side};

  if (bitmap_ != nullptr && bitmap_->valid) {
    // 圆角裁切：图标源图是方形，桌面语义要圆角（见头文件说明）。
    canvas.push_clip_rounded_rect(square, radius);
    // 不可用时**降饱和到灰**而不是降低不透明度：半透明的图标在深色壁纸上
    // 会读成"正在加载"，而灰阶明确表达"不可用"。
    const DrawOptions options{.opacity = 1.0f};
    if (dimmed_) {
      // 先贴图，再盖一层同形状的深灰（相乘无法在 Surface 上表达，
      // 用混合近似：55% 的深色足以把彩色压成"暗且无彩"）。
      canvas.draw_canvas(bitmap_->canvas, square, options);
      canvas.fill_rect(square, Paint::solid(Color{0x18, 0x1C, 0x24, 0xB0}), radius);
    } else {
      canvas.draw_canvas(bitmap_->canvas, square, options);
    }
    canvas.pop_clip();
    return;
  }

  // —— 占位：中性圆角块 + 首字母（见头文件"缺失时的姿态"） ——
  const Color plate = dimmed_ ? Color{0x2A, 0x2F, 0x3A, 0xFF} : Color{0x3A, 0x44, 0x5C, 0xFF};
  canvas.fill_rect(square, Paint::solid(plate), radius);
  if (glyph_.empty()) return;
  // 不自己排版文字：占位只画一个实心小方块，够表达"这里有个应用"。
  // 为什么不上 `TextRenderer`：本元素的绘制发生在任意 DPI 下，而取字形要走
  // `RenderContext` 的文字端口——为一块**降级占位**接那条链，成本远大于收益。
  const float inner = side * 0.34f;
  const Rect dot{square.x + (side - inner) * 0.5f, square.y + (side - inner) * 0.5f, inner, inner};
  canvas.fill_rect(dot, Paint::solid(Color{0xE8, 0xEE, 0xFA,
                                          static_cast<std::uint8_t>(dimmed_ ? 0x80 : 0xFF)}),
                   inner * 0.28f);
}

}  // namespace stdd
