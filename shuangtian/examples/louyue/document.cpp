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

auto PixelPatchCommand::try_merge(const Document& document, const PixelPatchCommand& next)
    -> bool {
  if (layer_ != next.layer_) return false;
  if (label_ != next.label_) return false;
  if (layer_ >= document.layers().size()) return false;
  // 相邻判定：矩形**扩张 1px** 后再问是否相交。
  //
  // 为何要扩张：相邻两次移动的可见像素可能正好接壤但不重叠（0 间隙），
  // 只判相交会把一整笔断成两段；扩张 1px 把"接壤"也认作连续。
  const Rect grown{area_.x - 1.0f, area_.y - 1.0f, area_.width + 2.0f, area_.height + 2.0f};
  // `Rect::intersect` 返回**交集矩形**（不是 bool）；空交集 = 不相邻。
  if (grown.intersect(next.area_).is_empty()) return false;

  // 并集矩形（`Rect` 的字段是 float，几何计算一律走 float，
  // 只在最后落到整数像素时才取整——混用 int/float 会产生一堆窄化告警，
  // 而它们掩盖真正的问题）。
  const Rect merged = Rect::from_ltrb(
      std::min(area_.x, next.area_.x), std::min(area_.y, next.area_.y),
      std::max(area_.right(), next.area_.right()),
      std::max(area_.bottom(), next.area_.bottom()));

  // `before_` 的拼法（**逐像素**，不是整块重取）：
  //
  // 并集内每个像素的"本笔开始前的值"来自三处之一：
  //   ① 在旧 `area_` 内 → 旧 `before_`（那是权威的"这笔开始前"）；
  //   ② 只在 `next.area_` 内 → `next.before_`（next 落笔前该处的样子）；
  //   ③ 两块都不覆盖（接壤时的缝）→ **当前画布值**（本笔没碰过，当前 == 开始前）。
  //
  // ②用 `next.before_` 而不是当前画布值：next 刚把那里改了，用当前值会把
  // "改动后"当成"原样"——撤销时留下一截笔迹（这类合并最典型的静默错）。
  const auto& canvas = document.layers()[layer_].pixels;
  std::vector<std::uint8_t> merged_before = read_region(canvas, merged);
  if (merged_before.empty()) return false;   // 并集完全在画布外：不并

  // 并集的**整数像素矩形**（与 `read_region` 同一套钳制；宽高显式算出来，
  // 不从字节数反推——反推出来的宽高在边界情况下会错，而错了就是整体错位）。
  const int mx0 = std::max(0, static_cast<int>(std::floor(merged.x)));
  const int my0 = std::max(0, static_cast<int>(std::floor(merged.y)));
  const int mx1 = std::min(canvas.physical_width(),
                           static_cast<int>(std::ceil(merged.right())));
  const int my1 = std::min(canvas.physical_height(),
                           static_cast<int>(std::ceil(merged.bottom())));
  if (mx1 <= mx0 || my1 <= my0) return false;
  const std::size_t merged_width = static_cast<std::size_t>(mx1 - mx0);
  const std::size_t merged_height = static_cast<std::size_t>(my1 - my0);

  const auto stamp = [&](const Rect& source_rect, const std::vector<std::uint8_t>& source) {
    if (source.empty()) return;
    const int sx0 = std::max(0, static_cast<int>(std::floor(source_rect.x)));
    const int sy0 = std::max(0, static_cast<int>(std::floor(source_rect.y)));
    const int sx1 = std::min(canvas.physical_width(),
                             static_cast<int>(std::ceil(source_rect.right())));
    const int sy1 = std::min(canvas.physical_height(),
                             static_cast<int>(std::ceil(source_rect.bottom())));
    if (sx1 <= sx0 || sy1 <= sy0) return;
    const int source_width = sx1 - sx0;
    const int source_height = sy1 - sy0;
    if (source.size() != static_cast<std::size_t>(source_width) * source_height * 4U) return;
    for (int y = 0; y < source_height; ++y) {
      for (int x = 0; x < source_width; ++x) {
        const int target_x = sx0 + x - mx0;
        const int target_y = sy0 + y - my0;
        if (target_x < 0 || target_y < 0 || target_x >= static_cast<int>(merged_width) ||
            target_y >= static_cast<int>(merged_height)) {
          continue;
        }
        const std::size_t from = (static_cast<std::size_t>(y) * source_width + x) * 4U;
        const std::size_t to =
            (static_cast<std::size_t>(target_y) * merged_width +
             static_cast<std::size_t>(target_x)) *
            4U;
        for (std::size_t channel = 0; channel < 4U; ++channel) {
          merged_before[to + channel] = source[from + channel];
        }
      }
    }
  };
  // ⚠ 顺序：先贴 next 的 before，再贴旧的 before。旧 before 更"早"
  //（它记录整笔开始前的样子），重叠处必须以它为准——反过来会让撤销停在中途。
  stamp(next.area_, next.before_);
  stamp(area_, before_);

  // `after_` 同样要**逐像素拼**，不能直接取 `next.after_`。
  //
  // 原因对称于 `before_`：并集矩形比 `next.area_` 大，而更大的那部分像素的
  // "这一笔之后的样子"只存在于**当前画布**上（旧 after 的那部分已被 last 覆盖）。
  // 直接取 `next.after_` 会让重做时只画出最后一小段——表现是"撤销再重做，笔迹变短了"。
  //
  // 填当前画布值是对的：并集内此刻的状态就是"这一笔进行到此为止"的样子，
  // 而重做要恢复的正是这个状态。
  std::vector<std::uint8_t> merged_after = read_region(canvas, merged);
  if (merged_after.empty()) return false;

  area_ = merged;
  before_ = std::move(merged_before);
  after_ = std::move(merged_after);
  return true;
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
  // **一笔 = 一条历史**：尝试把新补丁并进栈顶那一条。
  //
  // 为何在这一层做而不是让 `CanvasView` 自己攒：攒的逻辑要判断"两次移动是不是
  // 同一笔"，而那需要知道"图层/标签/位置是否连续"——那些只有补丁自己清楚。
  // 放在 `push` 里还有个好处：**所有**推入路径都自动获得合并
  //（画笔、橡皮、将来别的连续绘制），不必每处记得调。
  //
  // ⚠ 只在**栈顶是同类补丁**时并：光标不在末尾（用户撤销过）时说明新命令属于
  // 新的一段，与栈顶无关——那种情况下并进去会让"撤销一次退到别的地方"。
  if (!stroke_sealed_ && cursor_ == commands_.size() && cursor_ > 0U && !commands_.empty()) {
    if (auto* top = dynamic_cast<PixelPatchCommand*>(commands_[cursor_ - 1].get());
        top != nullptr) {
      if (auto* incoming = dynamic_cast<PixelPatchCommand*>(command.get());
          incoming != nullptr) {
        if (top->try_merge(*this, *incoming)) {
          // 已吸收：**不再新增条目**（历史条数不增 = 一笔一条）。
          dirty_ = true;
          return;
        }
        // 并**不进去**（不相邻/不同工具）也要**封笔**：新补丁成为新的一条，
        // 而"这一笔"就此断开——否则后面来的补丁会试图并进这条全新的命令，
        // 把两条无关的笔画连起来。
        stroke_sealed_ = true;
      }
    }
  }
  // 在历史中间落新命令 → 丢弃"未来"（与所有编辑器的语义一致）。
  if (cursor_ < commands_.size()) {
    commands_.resize(cursor_);
  }
  commands_.push_back(std::move(command));
  ++cursor_;
  // 落了一条新命令 = 本笔开始（后续同笔补丁可并进它），直到 `seal_stroke`。
  stroke_sealed_ = false;
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
