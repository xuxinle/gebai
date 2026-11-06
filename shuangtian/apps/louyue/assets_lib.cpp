/// 素材库的实现（见 `assets_lib.hpp`）。

#include "assets_lib.hpp"

#include <algorithm>
#include <format>

#include "st/core/fs.hpp"
#include "st/raster/paint.hpp"

namespace louyue {

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::DrawOptions;
using st::raster::Canvas;

[[nodiscard]] auto lower_ascii(std::string text) -> std::string {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

[[nodiscard]] auto extension_of(const std::string& path) -> std::string {
  const std::size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= path.size()) return {};
  return lower_ascii(path.substr(dot + 1));
}

[[nodiscard]] auto stem_of(const std::string& path) -> std::string {
  const std::size_t slash = path.find_last_of("/\\");
  const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
  const std::size_t dot = leaf.find_last_of('.');
  return dot == std::string::npos ? leaf : leaf.substr(0, dot);
}

/// PNG → Canvas（逐像素写；素材图通常几百 KB，一次解码可接受）。
[[nodiscard]] auto canvas_from_png(const st::codec::PngImage& image) -> Canvas {
  const int w = static_cast<int>(image.width);
  const int h = static_cast<int>(image.height);
  Canvas canvas{w, h, 1.0f};
  if (image.rgba.size() == static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U) {
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const std::size_t at =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4U;
        canvas.set_pixel(x, y, Color{image.rgba[at], image.rgba[at + 1], image.rgba[at + 2],
                                     image.rgba[at + 3]});
      }
    }
  }
  return canvas;
}

}  // namespace

void AssetLibrary::set_root(std::string root) {
  root_ = std::move(root);
  entries_.clear();
  cache_.clear();
  if (!root_.empty()) (void)st::fs::create_directories(root_);
}

auto AssetLibrary::refresh() -> std::size_t {
  entries_.clear();
  cache_.clear();
  if (root_.empty()) return 0;
  const auto listed = st::fs::list_dir(root_);
  if (!listed) return 0;
  for (const auto& item : listed.value()) {
    if (item.is_dir) continue;
    const std::string ext = extension_of(item.path);
    if (ext != "png") continue;   // 镂月只吃 PNG：解码器只有 png（不假装支持 jpg）
    AssetEntry entry{};
    entry.name = stem_of(item.path);
    entry.path = item.path;
    entry.bytes = static_cast<std::uint64_t>(item.size);
    if (const auto image = this->image(entry.path); image) {
      entry.width = image->width();
      entry.height = image->height();
      const int step = std::max(1, std::min(image->width(), image->height()) / 32);
      std::uint64_t r = 0, g = 0, b = 0, n = 0;
      for (int y = 0; y < image->height(); y += step) {
        for (int x = 0; x < image->width(); x += step) {
          const Color c = image->pixel_at(x, y);
          r += c.r;
          g += c.g;
          b += c.b;
          ++n;
        }
      }
      entry.average_color =
          n == 0 ? Color{0, 0, 0, 255}
                 : Color{static_cast<std::uint8_t>(r / n), static_cast<std::uint8_t>(g / n),
                         static_cast<std::uint8_t>(b / n), 255};
    }
    entries_.push_back(std::move(entry));
  }
  std::sort(entries_.begin(), entries_.end(),
            [](const AssetEntry& a, const AssetEntry& b) { return a.name < b.name; });
  return entries_.size();
}

auto AssetLibrary::find(std::string_view name) const -> const AssetEntry* {
  const std::string want = lower_ascii(std::string(name));
  for (const AssetEntry& entry : entries_) {
    if (lower_ascii(entry.name) == want) return &entry;
  }
  for (const AssetEntry& entry : entries_) {
    if (lower_ascii(stem_of(entry.path) + ".png") == want) return &entry;
  }
  return nullptr;
}

auto AssetLibrary::resolve(std::string_view ref, std::string_view basis) const
    -> std::optional<std::string> {
  if (ref.empty()) return std::nullopt;
  const std::string text(ref);
  if (st::fs::is_absolute(text)) {
    return st::fs::exists(text) ? std::optional<std::string>(text) : std::nullopt;
  }
  if (!basis.empty()) {
    const std::string joined = st::fs::join(std::string(basis), text);
    if (st::fs::exists(joined)) return joined;
  }
  if (const AssetEntry* entry = find(text); entry != nullptr) return entry->path;
  if (st::fs::exists(text)) return text;
  return std::nullopt;
}

auto AssetLibrary::image(std::string_view path) const -> std::shared_ptr<Canvas> {
  const std::string key(path);
  for (const auto& item : cache_) {
    if (item.first == key) return item.second;
  }
  const auto decoded = st::codec::png_read_file(key);
  if (!decoded) return nullptr;
  auto canvas = std::make_shared<Canvas>(canvas_from_png(decoded.value()));
  cache_.emplace_back(key, canvas);
  return canvas;
}

auto AssetLibrary::manifest() const -> std::string {
  std::string out;
  for (const AssetEntry& entry : entries_) {
    if (!out.empty()) out.push_back('\n');
    out += std::format("{}  {}x{}  {}", entry.name, entry.width, entry.height, entry.path);
  }
  return out;
}

void blit_cover(Canvas& target, const Canvas& source, Rect area, float alpha,
                float corner_radius, Color tint, float tint_alpha) {
  if (source.width() <= 0 || source.height() <= 0) return;
  if (area.width <= 0.0f || area.height <= 0.0f) return;
  auto& canvas = (target);
  const float target_ratio = area.width / area.height;
  const float source_ratio =
      static_cast<float>(source.width()) / static_cast<float>(source.height());
  (void)target_ratio;
  (void)source_ratio;
  DrawOptions options{};
  options.opacity = alpha;
  if (corner_radius > 0.5f) {
    canvas.push_clip_rounded_rect(area, corner_radius);
  } else {
    canvas.push_clip_rect(area);
  }
  canvas.draw_canvas(source, area, options);
  // 叠色：压暗/统一品牌色，让图上的文字可读（`tint_alpha` 为 0 时跳过）。
  if (tint_alpha > 0.001f) {
    canvas.fill_rect(area, st::raster::Paint::solid(Color{tint.r, tint.g, tint.b,
                                                          static_cast<std::uint8_t>(
                                                              static_cast<float>(tint.a) * tint_alpha)}));
  }
  canvas.pop_clip();
}

void blit_contain(Canvas& target, const Canvas& source, Rect area, float alpha,
                  float corner_radius) {
  if (source.width() <= 0 || source.height() <= 0) return;
  if (area.width <= 0.0f || area.height <= 0.0f) return;
  const float scale = std::min(area.width / static_cast<float>(source.width()),
                               area.height / static_cast<float>(source.height()));
  const float w = static_cast<float>(source.width()) * scale;
  const float h = static_cast<float>(source.height()) * scale;
  const Rect fitted{area.x + (area.width - w) * 0.5f, area.y + (area.height - h) * 0.5f, w, h};
  auto& canvas = (target);
  DrawOptions options{};
  options.opacity = alpha;
  if (corner_radius > 0.5f) {
    canvas.push_clip_rounded_rect(area, corner_radius);
  } else {
    canvas.push_clip_rect(area);
  }
  canvas.draw_canvas(source, fitted, options);
  canvas.pop_clip();
}

}  // namespace louyue
