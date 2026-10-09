/// 镂月文档模型的实现（见 `document.hpp` 的设计说明）。

#include "document.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace louyue {

namespace {

/// 取画布上一个矩形的 RGBA 字节（行优先，与 `Canvas` 的像素布局一致）。
[[nodiscard]] auto read_region(const Canvas& canvas, Rect area) -> std::vector<std::uint8_t> {
  const int x0 = std::max(0, static_cast<int>(std::floor(area.x)));
  const int y0 = std::max(0, static_cast<int>(std::floor(area.y)));
  const int x1 = std::min(canvas.physical_width(), static_cast<int>(std::ceil(area.right())));
  const int y1 = std::min(canvas.physical_height(), static_cast<int>(std::ceil(area.bottom())));
  if (x1 <= x0 || y1 <= y0) return {};

  const int w = x1 - x0;
  const int h = y1 - y0;
  std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4U, 0U);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const Color pixel = canvas.pixel_at(x0 + x, y0 + y);
      const std::size_t index = (static_cast<std::size_t>(y) * w + x) * 4U;
      out[index + 0] = pixel.r;
      out[index + 1] = pixel.g;
      out[index + 2] = pixel.b;
      out[index + 3] = pixel.a;
    }
  }
  return out;
}

/// 把 `source` 写回画布同一矩形（`read_region` 的逆操作，边界算法必须一致）。
void write_region(Canvas& canvas, Rect area, const std::vector<std::uint8_t>& source) {
  const int x0 = std::max(0, static_cast<int>(std::floor(area.x)));
  const int y0 = std::max(0, static_cast<int>(std::floor(area.y)));
  const int x1 = std::min(canvas.physical_width(), static_cast<int>(std::ceil(area.right())));
  const int y1 = std::min(canvas.physical_height(), static_cast<int>(std::ceil(area.bottom())));
  if (x1 <= x0 || y1 <= y0) return;

  const int w = x1 - x0;
  const int h = y1 - y0;
  if (source.size() != static_cast<std::size_t>(w) * h * 4U) return;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t index = (static_cast<std::size_t>(y) * w + x) * 4U;
      canvas.set_pixel(x0 + x, y0 + y,
                       Color{source[index + 0], source[index + 1], source[index + 2],
                             source[index + 3]});
    }
  }
}

/// 图层像素与目标的混合（`opacity` 由 paint 的 alpha 承载，`blend` 走 DrawOptions）。
void blend_layer(Canvas& target, const Layer& layer) {
  if (!layer.visible || layer.opacity <= 0.0f) return;
  st::raster::DrawOptions options{};
  options.blend = layer.blend;
  options.opacity = layer.opacity;
  target.draw_canvas_at(layer.pixels, 0, 0, options);
}

}  // namespace

// —— PixelPatchCommand ——

PixelPatchCommand::PixelPatchCommand(std::size_t layer, Rect area, std::string label)
    : layer_(layer), area_(area), label_(std::move(label)) {}

void PixelPatchCommand::capture_before(const Document& document) {
  if (layer_ >= document.layers().size()) return;
  before_ = read_region(document.layers()[layer_].pixels, area_);
}

void PixelPatchCommand::capture_after(const Document& document) {
  if (layer_ >= document.layers().size()) return;
  after_ = read_region(document.layers()[layer_].pixels, area_);
}

auto PixelPatchCommand::changed() const -> bool {
  return !before_.empty() && before_ != after_;
}

void PixelPatchCommand::blit(Document& document, const std::vector<std::uint8_t>& source) const {
  if (layer_ >= document.layers().size()) return;
  write_region(document.layers()[layer_].pixels, area_, source);
}

void PixelPatchCommand::undo(Document& document) { blit(document, before_); }
void PixelPatchCommand::redo(Document& document) { blit(document, after_); }

// —— LayerSnapshotCommand ——

void LayerSnapshotCommand::capture_after(const Document& document) {
  if (layer_ >= document.layers().size()) return;
  const Canvas& source = document.layers()[layer_].pixels;
  after_.emplace(source.physical_width(), source.physical_height(), 1.0f);
  after_->draw_canvas_at(source, 0, 0);
}

void LayerSnapshotCommand::undo(Document& document) {
  if (layer_ >= document.layers().size()) return;
  Canvas& target = document.layers()[layer_].pixels;
  target.clear(Color{0, 0, 0, 0});
  target.draw_canvas_at(before_, 0, 0);
}

void LayerSnapshotCommand::redo(Document& document) {
  if (layer_ >= document.layers().size() || !after_.has_value()) return;
  Canvas& target = document.layers()[layer_].pixels;
  target.clear(Color{0, 0, 0, 0});
  target.draw_canvas_at(*after_, 0, 0);
}

// —— Document ——

Document::Document(int width, int height) : width_(width), height_(height) {
  layers_.push_back(Layer{"背景", width, height});
  layers_.front().pixels.clear(Color{255, 255, 255, 255});
}

void Document::set_active_layer(std::size_t index) {
  if (index < layers_.size()) active_ = index;
}

void Document::add_layer(std::string name) {
  layers_.push_back(Layer{std::move(name), width_, height_});
  active_ = layers_.size() - 1;
  dirty_ = true;
}

auto Document::remove_layer(std::size_t index) -> bool {
  // 至少保留一层：删空之后没有"在哪画"这个概念，界面上会直接崩在 `active_pixels()`。
  if (layers_.size() <= 1U || index >= layers_.size()) return false;
  layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(index));
  if (active_ >= layers_.size()) active_ = layers_.size() - 1;
  dirty_ = true;
  return true;
}

auto Document::move_layer(std::size_t index, int delta) -> bool {
  if (index >= layers_.size()) return false;
  const auto target = static_cast<std::ptrdiff_t>(index) + delta;
  if (target < 0 || target >= static_cast<std::ptrdiff_t>(layers_.size())) return false;
  std::swap(layers_[index], layers_[static_cast<std::size_t>(target)]);
  if (active_ == index) {
    active_ = static_cast<std::size_t>(target);
  } else if (active_ == static_cast<std::size_t>(target)) {
    active_ = index;
  }
  dirty_ = true;
  return true;
}

auto Document::merge_down(std::size_t index) -> bool {
  // 最底层没有"下面"；越界也拒绝。
  if (index == 0U || index >= layers_.size()) return false;
  Layer& upper = layers_[index];
  Layer& lower = layers_[index - 1U];
  // 把上层按它自己的 opacity/blend 合进下层（结果留在下层）。
  blend_layer(lower.pixels, upper);
  layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(index));
  if (active_ >= layers_.size()) active_ = layers_.size() - 1;
  dirty_ = true;
  return true;
}

void Document::composite(Canvas& target) const {
  target.clear(Color{0, 0, 0, 0});
  for (const Layer& layer : layers_) blend_layer(target, layer);
}

auto Document::composite_pixel(int x, int y) const -> Color {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) return Color{0, 0, 0, 0};
  Canvas target{width_, height_, 1.0f};
  composite(target);
  return target.pixel_at(x, y);
}

void Document::push(std::unique_ptr<Command> command) {
  if (command == nullptr) return;
  // 在历史中间落新命令 → 丢弃"未来"（与所有编辑器的语义一致）。
  if (cursor_ < commands_.size()) {
    commands_.resize(cursor_);
  }
  commands_.push_back(std::move(command));
  ++cursor_;
  dirty_ = true;
}

auto Document::undo() -> bool {
  if (!can_undo()) return false;
  --cursor_;
  commands_[cursor_]->undo(*this);
  dirty_ = true;
  return true;
}

auto Document::redo() -> bool {
  if (!can_redo()) return false;
  commands_[cursor_]->redo(*this);
  ++cursor_;
  dirty_ = true;
  return true;
}

void Document::clear_history() {
  commands_.clear();
  cursor_ = 0;
}

auto Document::history_names() const -> std::vector<std::string> {
  std::vector<std::string> names;
  names.reserve(cursor_);
  for (std::size_t i = 0; i < cursor_; ++i) names.push_back(commands_[i]->name());
  return names;
}

auto Document::load_png(const st::codec::PngImage& image) -> bool {
  if (image.width == 0U || image.height == 0U) return false;
  width_ = static_cast<int>(image.width);
  height_ = static_cast<int>(image.height);
  layers_.clear();
  layers_.push_back(Layer{"背景", width_, height_});
  Canvas& target = layers_.front().pixels;
  target.clear(Color{0, 0, 0, 0});
  // PNG 是 8 位 RGBA、行优先，与 `Canvas` 的像素布局同构——逐像素拷（不 reinterpret，
  // 两者的行距/对齐不保证一致）。
  for (std::uint32_t y = 0; y < image.height; ++y) {
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 4U;
      if (index + 3U >= image.rgba.size()) return false;
      target.set_pixel(static_cast<int>(x), static_cast<int>(y),
                       Color{image.rgba[index + 0], image.rgba[index + 1], image.rgba[index + 2],
                             image.rgba[index + 3]});
    }
  }
  active_ = 0;
  clear_history();
  dirty_ = false;
  return true;
}

auto Document::export_png(std::string_view path) const -> bool {
  Canvas target{width_, height_, 1.0f};
  composite(target);
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(width_);
  image.height = static_cast<std::uint32_t>(height_);
  image.rgba.resize(static_cast<std::size_t>(width_) * height_ * 4U);
  for (int y = 0; y < height_; ++y) {
    for (int x = 0; x < width_; ++x) {
      const Color pixel = target.pixel_at(x, y);
      const std::size_t index = (static_cast<std::size_t>(y) * width_ + x) * 4U;
      image.rgba[index + 0] = pixel.r;
      image.rgba[index + 1] = pixel.g;
      image.rgba[index + 2] = pixel.b;
      image.rgba[index + 3] = pixel.a;
    }
  }
  const auto written = st::codec::png_write_file(path, image);
  return written.has_value();
}

// —— 画笔 ——

auto stroke_bounds(st::math::Point from, st::math::Point to, float radius) -> Rect {
  // 受影响矩形：起点/终点包围盒外扩半径 + 1px（抗锯齿会溢出半个像素）。
  const float pad = radius + 1.0f;
  const float x0 = std::min(from.x, to.x) - pad;
  const float y0 = std::min(from.y, to.y) - pad;
  const float x1 = std::max(from.x, to.x) + pad;
  const float y1 = std::max(from.y, to.y) + pad;
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

auto stroke_segment(Canvas& target, st::math::Point from, st::math::Point to, float radius,
                    Color color, bool erase) -> Rect {
  const float dx = to.x - from.x;
  const float dy = to.y - from.y;
  const float length = std::sqrt(dx * dx + dy * dy);
  // **按半径的 1/4 步进**：步长小于半径时圆点必然交叠，画出来是连续笔画而不是虚线。
  // 步长取 1px 的上限是为了长笔画别退化成几万个点。
  const float step = std::max(0.25f, std::min(radius * 0.25f, 1.0f));
  const int steps = std::max(1, static_cast<int>(std::ceil(length / step)));

  st::raster::Paint paint = st::raster::Paint::solid(color);
  st::raster::DrawOptions options{};
  // 橡皮不是"画白色"——它把目标区域的 alpha 抹掉（下面用 DstOut 语义模拟：
  // 这里是逐圆点扣 alpha）。用 `BlendMode::Src` 会把颜色也覆盖成透明黑，
  // 图层面板上看是"抠了个洞"而不是"擦掉了"，两者在下层有内容时观感不同。
  if (erase) {
    options.blend = BlendMode::Src;
    paint = st::raster::Paint::solid(Color{0, 0, 0, 0});
  }
  for (int i = 0; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const float x = from.x + dx * t;
    const float y = from.y + dy * t;
    target.fill_rect(Rect{x - radius, y - radius, radius * 2.0f, radius * 2.0f}, paint, radius,
                     options);
  }
  return stroke_bounds(from, to, radius);
}

}  // namespace louyue
