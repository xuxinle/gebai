#include "st/raster/glyph_atlas_content.hpp"

#include <algorithm>
#include <cstring>

namespace st::raster::atlas {

GlyphAtlas::GlyphAtlas(int initial_dimension, int max_dimension)
    : max_dimension_(std::max(initial_dimension, max_dimension)) {
  const int side = std::max(16, initial_dimension);
  packer_.set_canvas(side, side);
  pixels_.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4U, 0U);
}

auto GlyphAtlas::place(std::uint64_t key, int w, int h,
                       const std::function<void(std::uint8_t*, int)>& write)
    -> std::optional<Placement> {
  if (w <= 0 || h <= 0) return std::nullopt;

  // ① 命中：完全不碰像素（这是稳态下 99% 的路径）
  if (const auto found = entries_.find(key); found != entries_.end()) {
    const Region& region = found->second;
    if (region.width == w && region.height == h) {
      return Placement{region, false};
    }
    // 尺寸不符（同一 key 换了字号？）：当作未命中，旧空间浪费掉不再回收。
    // 不回收是有意的——字形 key 含字号档，尺寸不同意味着真是不同条目。
  }

  // ② 分配（可能连续失败并扩容）
  auto region = packer_.allocate(w, h);
  while (!region.has_value()) {
    if (!grow()) return std::nullopt;   // 到上限了
    region = packer_.allocate(w, h);
  }

  // ③ 写入像素
  const int row_bytes = packer_.canvas_width() * 4;
  std::uint8_t* base = pixels_.data() + static_cast<std::size_t>(region->y) * row_bytes +
                       static_cast<std::size_t>(region->x) * 4U;
  if (write) write(base, row_bytes);

  entries_.emplace(key, *region);
  return Placement{*region, true};
}

auto GlyphAtlas::grow() -> bool {
  const int current_w = packer_.canvas_width();
  const int current_h = packer_.canvas_height();
  const auto next = next_capacity(current_w, current_h, max_dimension_);
  if (!next.has_value()) return false;
  const auto [next_w, next_h] = *next;

  // **保留旧像素并重新装箱**（而不是清空重来）：扩容本身不频繁，
  // 但清空会让“扩容后那一帧”把几百个字形全部重传一遍。
  struct Kept {
    std::uint64_t key;
    Region old;
  };
  std::vector<Kept> kept;
  kept.reserve(entries_.size());
  for (const auto& [key, region] : entries_) kept.push_back(Kept{key, region});

  // 用**旧**缓冲的拷贝做搬运源（下面会重建缓冲）。
  const std::vector<std::uint8_t> previous = pixels_;
  const int previous_row_bytes = current_w * 4;

  packer_.reset();
  packer_.set_canvas(next_w, next_h);
  pixels_.assign(static_cast<std::size_t>(next_w) * static_cast<std::size_t>(next_h) * 4U, 0U);
  entries_.clear();

  const int row_bytes = next_w * 4;
  for (const Kept& item : kept) {
    const auto region = packer_.allocate(item.old.width, item.old.height);
    if (!region.has_value()) continue;   // 理论上不会（画布只会变大），保险
    for (int row = 0; row < item.old.height; ++row) {
      const std::uint8_t* source = previous.data() +
                                   static_cast<std::size_t>(item.old.y + row) * previous_row_bytes +
                                   static_cast<std::size_t>(item.old.x) * 4U;
      std::uint8_t* destination =
          pixels_.data() + static_cast<std::size_t>(region->y + row) * row_bytes +
          static_cast<std::size_t>(region->x) * 4U;
      std::memcpy(destination, source, static_cast<std::size_t>(item.old.width) * 4U);
    }
    entries_.emplace(item.key, *region);
  }

  resized_ = true;
  ++generation_;
  return true;
}

void GlyphAtlas::reset() {
  packer_.reset();
  const int side = std::max(16, packer_.canvas_width());
  packer_.set_canvas(side, side);
  pixels_.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4U, 0U);
  entries_.clear();
  resized_ = true;
  ++generation_;
}

}  // namespace st::raster::atlas
