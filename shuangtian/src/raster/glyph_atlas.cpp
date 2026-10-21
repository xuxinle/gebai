#include "st/raster/glyph_atlas.hpp"

#include <algorithm>
#include <utility>

namespace st::raster::atlas {

auto ShelfPacker::allocate(int w, int h) -> std::optional<Region> {
  if (w <= 0 || h <= 0) return std::nullopt;
  if (canvas_width_ <= 0 || canvas_height_ <= 0) return std::nullopt;
  // 单个条目超过画布 → 永远放不下（不要白试一轮）。
  if (w > canvas_width_ || h > canvas_height_) return std::nullopt;

  // 当前货架放不下这个宽度的条目 → 开新货架。
  // 注意 `cursor_x_ > 0` 的判断：货架刚开（cursor 为 0）时不该再开新货架，
  // 否则一个比整行还宽的条目会无限开空货架。
  if (cursor_x_ > 0 && cursor_x_ + w > canvas_width_) {
    shelf_y_ += shelf_height_ + padding_;
    shelf_height_ = 0;
    cursor_x_ = 0;
  }

  // 高度不够：报失败，但**不改变已占用的状态**（调用方扩容后重试）。
  if (shelf_y_ + std::max(shelf_height_, h) > canvas_height_) return std::nullopt;

  Region region{cursor_x_, shelf_y_, w, h};
  cursor_x_ += w + padding_;
  shelf_height_ = std::max(shelf_height_, h);
  entries_.push_back(Entry{region});
  return region;
}

void ShelfPacker::reset() noexcept {
  shelf_y_ = 0;
  shelf_height_ = 0;
  cursor_x_ = 0;
  entries_.clear();
}

void ShelfPacker::set_canvas(int width, int height) noexcept {
  canvas_width_ = std::max(0, width);
  canvas_height_ = std::max(0, height);
}

auto next_capacity(int width, int height, int max_dimension) noexcept
    -> std::optional<std::pair<int, int>> {
  if (width <= 0 || height <= 0 || max_dimension <= 0) return std::nullopt;
  // 宽度优先翻倍：字形图集通常“宽 > 高”（一行放很多字）。
  if (width < max_dimension) {
    return std::pair{std::min(width * 2, max_dimension), height};
  }
  if (height < max_dimension) {
    return std::pair{width, std::min(height * 2, max_dimension)};
  }
  return std::nullopt;   // 两维都到上限：装不下了
}

}  // namespace st::raster::atlas
