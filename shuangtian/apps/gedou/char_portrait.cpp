#include "char_portrait.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

#include "st/codec/png.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"

namespace gedou {

namespace {

/// 把解码后的 PNG 像素搬进一块 `Canvas`（它同时是 `Surface`）。
///
/// 为什么中间要过 Canvas：`draw_canvas` 的源是 `Surface`，而 `Surface` 是抽象基类，
/// 具体实现就是这个 `Canvas`。所以解码后必须"灌"进一个 Canvas 才能当贴图画。
[[nodiscard]] auto make_surface(const st::codec::PngImage& image)
    -> std::unique_ptr<st::raster::Surface> {
  if (image.width == 0 || image.height == 0) return nullptr;
  auto canvas = std::make_unique<st::raster::Canvas>(static_cast<int>(image.width),
                                                     static_cast<int>(image.height));
  // `pixels()` 返回 `std::span<std::uint32_t>`（每个元素是一个 RGBA 像素，内存序 R<<24|G<<16|B<<8|A）。
  // PngImage 给的是逐字节 rgba，所以按 4 字节重解释后整块拷贝。
  auto destination = canvas->pixels();
  const std::size_t pixels = static_cast<std::size_t>(image.width) *
                             static_cast<std::size_t>(image.height);
  const std::size_t bytes = pixels * 4U;
  if (destination.size() < pixels || image.rgba.size() < bytes) return nullptr;
  std::memcpy(destination.data(), image.rgba.data(), bytes);
  return canvas;
}

/// 水平镜像一张 Surface（逐像素倒序拷贝）。
[[nodiscard]] auto mirror_surface(const st::raster::Surface& source)
    -> std::unique_ptr<st::raster::Surface> {
  const int width = source.physical_width();
  const int height = source.physical_height();
  if (width <= 0 || height <= 0) return nullptr;
  auto canvas = std::make_unique<st::raster::Canvas>(width, height);
  const auto from = source.pixels();
  auto to = canvas->pixels();
  const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (from.size() < pixels || to.size() < pixels) return nullptr;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
      to[row + static_cast<std::size_t>(x)] =
          from[row + static_cast<std::size_t>(width - 1 - x)];
    }
  }
  return canvas;
}

}  // namespace

auto CharacterPortrait::load(std::vector<unsigned char> bytes) -> CharacterPortrait {
  if (bytes.empty()) return {};
  const auto decoded =
      st::codec::png_decode(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
  if (!decoded.has_value()) {
    st::log::out("立绘解码失败：{}\n", decoded.error().to_string());
    return {};
  }
  CharacterPortrait portrait;
  portrait.surface_ = make_surface(*decoded);
  if (portrait.surface_ != nullptr) {
    portrait.mirrored_ = mirror_surface(*portrait.surface_);
  }
  return portrait;
}

auto CharacterPortrait::load_file(std::string_view path) -> CharacterPortrait {
  const auto decoded = st::codec::png_read_file(path);
  if (!decoded.has_value()) {
    st::log::out("立绘读取失败（{}）：{}\n", path, decoded.error().to_string());
    return {};
  }
  CharacterPortrait portrait;
  portrait.surface_ = make_surface(*decoded);
  if (portrait.surface_ != nullptr) {
    portrait.mirrored_ = mirror_surface(*portrait.surface_);
  }
  return portrait;
}

auto CharacterPortrait::width() const noexcept -> int {
  return surface_ != nullptr ? surface_->physical_width() : 0;
}

auto CharacterPortrait::height() const noexcept -> int {
  return surface_ != nullptr ? surface_->physical_height() : 0;
}

void CharacterPortrait::draw(st::raster::Surface& canvas, float center_x, float bottom_y,
                             float height, bool facing_right) const {
  if (surface_ == nullptr) return;
  const int source_w = surface_->physical_width();
  const int source_h = surface_->physical_height();
  if (source_w <= 0 || source_h <= 0) return;

  // 按目标高度等比缩放（宽度按原图比例推出，避免拉伸变形）
  const float scale = height / static_cast<float>(source_h);
  const float draw_w = static_cast<float>(source_w) * scale;
  const float draw_h = height;
  const math::Rect destination{center_x - draw_w * 0.5f, bottom_y - draw_h, draw_w, draw_h};

  // 镜像：立绘是朝正面画的，朝左时用预存的镜像副本（`DrawOptions` 没有翻转字段）。
  const st::raster::Surface* source = surface_.get();
  if (!facing_right && mirrored_ != nullptr) source = mirrored_.get();
  canvas.draw_canvas(*source, destination);
}

// ── ActionSprites ──

void ActionSprites::set(std::size_t action, CharacterPortrait portrait) {
  if (action >= kActionCount) return;
  if (portrait.valid()) any_valid_ = true;
  sprites_[action] = std::move(portrait);
}

auto ActionSprites::get(std::size_t action) const -> const CharacterPortrait& {
  static const CharacterPortrait kEmpty{};
  if (action < kActionCount && sprites_[action].valid()) return sprites_[action];
  // 回退到 idle（`MoveId::Idle` 的数值是 0）
  if (sprites_[0].valid()) return sprites_[0];
  return kEmpty;
}

}  // namespace gedou
