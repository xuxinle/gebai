/// 素材库的实现（见 `assets_lib.hpp`）。
///
/// 全文件用 float（设计坐标是像素量级）——与 `motion.cpp` 同一约定，
/// 避免 double-promotion（本仓 `-Werror`）。

#include "assets_lib.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "st/core/print.hpp"
#include "st/core/fs.hpp"
#include "st/raster/paint.hpp"

namespace caiyun {

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Paint;
using st::raster::Canvas;

[[nodiscard]] auto lower_ascii(std::string text) -> std::string {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

/// 扩展名（小写、不含点）。
[[nodiscard]] auto extension_of(const std::string& path) -> std::string {
  const std::size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= path.size()) return {};
  return lower_ascii(path.substr(dot + 1));
}

/// 不带扩展名的文件名。
[[nodiscard]] auto stem_of(const std::string& path) -> std::string {
  const std::size_t slash = path.find_last_of("/\\");
  const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
  const std::size_t dot = leaf.find_last_of('.');
  return dot == std::string::npos ? leaf : leaf.substr(0, dot);
}

[[nodiscard]] auto is_image_ext(const std::string& ext) -> bool {
  return ext == "png" || ext == "jpg" || ext == "jpeg";
}

[[nodiscard]] auto is_audio_ext(const std::string& ext) -> bool { return ext == "wav"; }

/// 把 PNG 解码结果变成 Surface。
[[nodiscard]] auto surface_from_png(const st::codec::PngImage& image) -> Canvas {
  Canvas canvas{static_cast<int>(image.width), static_cast<int>(image.height), 1.0f};
  if (image.rgba.size() == static_cast<std::size_t>(image.width) * image.height * 4U) {
    const int w = static_cast<int>(image.width);
    const int h = static_cast<int>(image.height);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                static_cast<std::size_t>(x)) *
                               4U;
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
  if (!root_.empty()) {
    (void)st::fs::create_directories(root_);
  }
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
    if (!is_image_ext(ext) && !is_audio_ext(ext)) continue;
    AssetEntry entry{};
    entry.name = stem_of(item.path);
    entry.path = item.path;
    entry.ext = ext;
    entry.bytes = static_cast<std::uint64_t>(item.size);
    if (is_image_ext(ext)) {
      const auto image = this->image(entry.path);
      if (image) {
        entry.width = image->width();
        entry.height = image->height();
        // 主色：缩到 1×1 采样太粗，取中心 32×32 区域均值——足以代表画面基调。
        entry.average_color = [&] {
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
          if (n == 0) return Color{0, 0, 0, 255};
          return Color{static_cast<std::uint8_t>(r / n), static_cast<std::uint8_t>(g / n),
                       static_cast<std::uint8_t>(b / n), 255};
        }();
      }
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
  // 也接受带扩展名的全名（`cover.png`）。
  for (const AssetEntry& entry : entries_) {
    const std::string leaf = stem_of(entry.path) + "." + entry.ext;
    if (lower_ascii(leaf) == want) return &entry;
  }
  return nullptr;
}

auto AssetLibrary::resolve(std::string_view ref, std::string_view basis) const
    -> std::optional<std::string> {
  if (ref.empty()) return std::nullopt;
  const std::string text(ref);
  // ① 绝对路径
  if (st::fs::is_absolute(text)) {
    if (st::fs::exists(text)) return text;
    return std::nullopt;
  }
  // ② 相对脚本目录
  if (!basis.empty()) {
    const std::string joined = st::fs::join(std::string(basis), text);
    if (st::fs::exists(joined)) return joined;
  }
  // ③ 素材库内名称（`cover` 或 `cover.png`）
  if (const AssetEntry* entry = find(text); entry != nullptr) return entry->path;
  // ④ 相对当前工作目录（命令行里手写路径时常见）
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
  auto canvas = std::make_shared<Canvas>(surface_from_png(decoded.value()));
  cache_.emplace_back(key, canvas);
  return canvas;
}

auto AssetLibrary::manifest() const -> std::string {
  std::string out;
  for (const AssetEntry& entry : entries_) {
    if (!out.empty()) out.push_back('\n');
    if (entry.width > 0) {
      out += std::format("{}  {}x{}  {}", entry.name, entry.width, entry.height, entry.path);
    } else {
      out += std::format("{}  [{}]  {}", entry.name, entry.ext, entry.path);
    }
  }
  return out;
}

void blit_cover(Canvas& target, const Canvas& source, Rect area, float alpha,
                float corner_radius) {
  if (source.width() <= 0 || source.height() <= 0) return;
  if (area.width <= 0.0f || area.height <= 0.0f) return;
  auto& canvas = target;
  st::raster::DrawOptions options{};
  options.opacity = alpha;
  if (corner_radius > 0.5f) {
    canvas.push_clip_rounded_rect(area, corner_radius);
  } else {
    canvas.push_clip_rect(area);
  }

  // ① **真正做 cover**：先按目标长宽比在源上取最大居中区域，
  //    再把这块区域贴满目标框。早先这里算了 src 却没用（直接整幅拉伸）——
  //    结果源与目标比例不一致时画面被拉变形（人脸/产品一眼假）。
  const float target_ratio = area.width / area.height;
  const float source_ratio =
      static_cast<float>(source.width()) / static_cast<float>(source.height());
  Rect src{0.0f, 0.0f, static_cast<float>(source.width()), static_cast<float>(source.height())};
  if (source_ratio > target_ratio) {
    src.width = static_cast<float>(source.height()) * target_ratio;
    src.x = (static_cast<float>(source.width()) - src.width) * 0.5f;
  } else if (source_ratio < target_ratio) {
    src.height = static_cast<float>(source.width()) / target_ratio;
    src.y = (static_cast<float>(source.height()) - src.height) * 0.5f;
  }

  // ② **分步放大**：素材（768×512）铺 1920×1080 要放 2.5 倍，而 `draw_canvas`
  //    是双线性——一次大步放大明显发糊。先缩到目标尺寸的 1/2、再交给双线性，
  //    两次小步比一次大步清晰得多（比实现 Lanczos 便宜很多）。
  const float scale_x = area.width / std::max(1.0f, src.width);
  Canvas staged{2, 2, 1.0f};
  bool use_stage = false;
  if (scale_x > 1.6f) {
    const int mid_w = std::clamp(static_cast<int>(area.width * 0.5f), 1, 4096);
    const int mid_h = std::clamp(static_cast<int>(area.height * 0.5f), 1, 4096);
    staged = Canvas{mid_w, mid_h, 1.0f};
    st::raster::DrawOptions stage_options{};
    stage_options.opacity = 1.0f;
    // 先把源**裁切区**映射到整幅 intermediate（这里用整幅源 + 目标比例换算）。
    staged.draw_canvas(source,
                       Rect{-src.x * static_cast<float>(mid_w) / src.width,
                            -src.y * static_cast<float>(mid_h) / src.height,
                            static_cast<float>(source.width()) * static_cast<float>(mid_w) / src.width,
                            static_cast<float>(source.height()) * static_cast<float>(mid_h) / src.height},
                       stage_options);
    use_stage = true;
  }
  if (use_stage) {
    canvas.draw_canvas(staged, area, options);
  } else {
    canvas.draw_canvas(source, area, options);
  }
  canvas.pop_clip();
}

void blit_contain(Canvas& target, const Canvas& source, Rect area, float alpha) {
  if (source.width() <= 0 || source.height() <= 0) return;
  if (area.width <= 0.0f || area.height <= 0.0f) return;
  const float scale = std::min(area.width / static_cast<float>(source.width()),
                               area.height / static_cast<float>(source.height()));
  const float w = static_cast<float>(source.width()) * scale;
  const float h = static_cast<float>(source.height()) * scale;
  const Rect fitted{area.x + (area.width - w) * 0.5f, area.y + (area.height - h) * 0.5f, w, h};
  auto& canvas = (target);
  st::raster::DrawOptions options{};
  options.opacity = alpha;
  canvas.push_clip_rect(area);
  canvas.draw_canvas(source, fitted, options);
  canvas.pop_clip();
}

}  // namespace caiyun
