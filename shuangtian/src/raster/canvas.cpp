#include "st/raster/canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "rasterize_internal.hpp"
#include "st/raster/simd.hpp"

namespace st::raster {
namespace {

inline constexpr float kCoverageEpsilon = 0.0005f;

/// 预乘像素分量解包（位布局：R<<24 | G<<16 | B<<8 | A）。
[[nodiscard]] constexpr auto channel(std::uint32_t pixel, unsigned shift) noexcept -> std::uint32_t {
  return (pixel >> shift) & 0xFFU;
}

[[nodiscard]] constexpr auto pack(std::uint32_t red, std::uint32_t green, std::uint32_t blue,
                                  std::uint32_t alpha) noexcept -> std::uint32_t {
  return (red << 24U) | (green << 16U) | (blue << 8U) | alpha;
}

/// 预乘源覆盖（源已按 alpha 缩放）。
[[nodiscard]] constexpr auto over_premul(std::uint32_t dst, std::uint32_t src) noexcept
    -> std::uint32_t {
  const std::uint32_t sa = channel(src, 0);
  if (sa == 0U) return dst;
  const std::uint32_t inverse = 255U - sa;
  const auto mix = [inverse](std::uint32_t source, std::uint32_t destination) constexpr noexcept {
    return source + (destination * inverse + 127U) / 255U;
  };
  return pack(mix(channel(src, 24), channel(dst, 24)), mix(channel(src, 16), channel(dst, 16)),
              mix(channel(src, 8), channel(dst, 8)), mix(sa, channel(dst, 0)));
}

/// 按覆盖率缩放预乘源。
[[nodiscard]] constexpr auto scale_premul(std::uint32_t src, std::uint32_t alpha) noexcept
    -> std::uint32_t {
  const auto scale = [alpha](std::uint32_t value) constexpr noexcept -> std::uint32_t {
    return (value * alpha + 127U) / 255U;
  };
  return pack(scale(channel(src, 24)), scale(channel(src, 16)), scale(channel(src, 8)),
              scale(channel(src, 0)));
}

/// 直通 α 混合算子（非 SrcOver 模式在直通空间计算）。
[[nodiscard]] constexpr auto blend_channel(BlendMode mode, std::uint32_t backdrop,
                                           std::uint32_t source) noexcept -> std::uint32_t {
  switch (mode) {
    case BlendMode::Multiply: return (backdrop * source + 127U) / 255U;
    case BlendMode::Screen: return backdrop + source - (backdrop * source + 127U) / 255U;
    case BlendMode::Darken: return backdrop < source ? backdrop : source;
    case BlendMode::Lighten: return backdrop > source ? backdrop : source;
    case BlendMode::Add: {
      const std::uint32_t sum = backdrop + source;
      return sum > 255U ? 255U : sum;
    }
    case BlendMode::Overlay: {
      const auto overlay = [](std::uint32_t base, std::uint32_t top) constexpr noexcept {
        return base < 128U ? (2U * base * top + 127U) / 255U
                           : 255U - 2U * (255U - base) * (255U - top) / 255U;
      };
      return overlay(backdrop, source);
    }
    case BlendMode::DstOver:
    case BlendMode::SrcOver:
    case BlendMode::Src: return source;
  }
  return source;
}

/// 通用像素混合（`coverage` ∈ [0,1] 已含 opacity 与裁剪遮罩）。
[[nodiscard]] auto blend_pixel(std::uint32_t dst, math::Color color, float coverage,
                               BlendMode mode) noexcept -> std::uint32_t {
  const float clamped = coverage <= 1.0f ? coverage : 1.0f;
  const auto alpha_byte = static_cast<std::uint32_t>(clamped * 255.0f + 0.5f);
  const std::uint32_t source_premul = math::premultiply(color);
  const std::uint32_t scaled = scale_premul(source_premul, alpha_byte);

  if (mode == BlendMode::Src) return scaled;
  if (mode == BlendMode::SrcOver) return over_premul(dst, scaled);
  if (mode == BlendMode::DstOver) {
    const std::uint32_t dst_alpha = channel(dst, 0);
    const std::uint32_t inverse = 255U - dst_alpha;
    return pack(std::min(255U, channel(dst, 24) + channel(scaled, 24) * inverse / 255U),
                std::min(255U, channel(dst, 16) + channel(scaled, 16) * inverse / 255U),
                std::min(255U, channel(dst, 8) + channel(scaled, 8) * inverse / 255U),
                std::min(255U, dst_alpha + channel(scaled, 0) * inverse / 255U));
  }

  // 其余模式：背景与源都转直通 → 逐通道混合 → 再按源 alpha 覆盖
  const math::Color backdrop = math::unpremultiply(dst);
  const bool has_backdrop = backdrop.a != 0U;
  std::uint32_t blended = 0;
  if (has_backdrop) {
    const auto to_byte = [](std::uint8_t value) noexcept -> std::uint32_t {
      return static_cast<std::uint32_t>(value);
    };
    blended = pack(blend_channel(mode, to_byte(backdrop.r), to_byte(color.r)),
                   blend_channel(mode, to_byte(backdrop.g), to_byte(color.g)),
                   blend_channel(mode, to_byte(backdrop.b), to_byte(color.b)), 255U);
    blended = math::premultiply(math::unpremultiply(blended).with_alpha(color.a));
  } else {
    blended = scale_premul(source_premul, 255U);
  }
  return over_premul(dst, scale_premul(blended, alpha_byte));
}

/// 盒式模糊（单次，水平或垂直）。
void box_blur_pass(std::span<std::uint8_t> values, int width, int height, int radius,
                   bool horizontal) {
  if (radius <= 0) return;
  const int outer = horizontal ? height : width;
  const int inner = horizontal ? width : height;
  std::vector<std::uint32_t> line(static_cast<std::size_t>(inner));
  for (int index = 0; index < outer; ++index) {
    for (int position = 0; position < inner; ++position) {
      const int x = horizontal ? position : index;
      const int y = horizontal ? index : position;
      line[static_cast<std::size_t>(position)] =
          values[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)];
    }
    std::uint32_t sum = 0;
    for (int offset = -radius; offset <= radius; ++offset) {
      const int clamped = std::clamp(offset, 0, inner - 1);
      sum += line[static_cast<std::size_t>(clamped)];
    }
    const auto divisor = static_cast<std::uint32_t>(2 * radius + 1);
    for (int position = 0; position < inner; ++position) {
      const int x = horizontal ? position : index;
      const int y = horizontal ? index : position;
      values[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
             static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(sum / divisor);
      const int leaving = std::clamp(position - radius, 0, inner - 1);
      const int entering = std::clamp(position + radius + 1, 0, inner - 1);
      sum -= line[static_cast<std::size_t>(leaving)];
      sum += line[static_cast<std::size_t>(entering)];
    }
  }
}

}  // namespace

// —— Mask ——

Mask::Mask(int width, int height)
    : width_(width > 0 ? width : 0), height_(height > 0 ? height : 0),
      values_(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0U) {}

auto Mask::at(int x, int y) const noexcept -> std::uint8_t {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) return 0U;
  return values_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                 static_cast<std::size_t>(x)];
}

void Mask::set(int x, int y, std::uint8_t value) noexcept {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
  values_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
          static_cast<std::size_t>(x)] = value;
}

auto Mask::values() noexcept -> std::span<std::uint8_t> { return values_; }
auto Mask::values() const noexcept -> std::span<const std::uint8_t> { return values_; }

auto Mask::sample(float x, float y) const noexcept -> float {
  if (width_ <= 0 || height_ <= 0) return 0.0f;
  const float clamped_x = math::clampf(x - 0.5f, 0.0f, static_cast<float>(width_ - 1));
  const float clamped_y = math::clampf(y - 0.5f, 0.0f, static_cast<float>(height_ - 1));
  const auto x0 = static_cast<int>(clamped_x);
  const auto y0 = static_cast<int>(clamped_y);
  const int x1 = x0 + 1 < width_ ? x0 + 1 : x0;
  const int y1 = y0 + 1 < height_ ? y0 + 1 : y0;
  const float fx = clamped_x - static_cast<float>(x0);
  const float fy = clamped_y - static_cast<float>(y0);
  const auto top = static_cast<float>(at(x0, y0)) + (static_cast<float>(at(x1, y0)) - static_cast<float>(at(x0, y0))) * fx;
  const auto bottom = static_cast<float>(at(x0, y1)) + (static_cast<float>(at(x1, y1)) - static_cast<float>(at(x0, y1))) * fx;
  return (top + (bottom - top) * fy) / 255.0f;
}

void blur_mask(Mask& mask, float radius) {
  if (mask.empty() || radius <= 0.5f) return;
  const auto box_radius = static_cast<int>(std::lround(radius * 0.5f));
  if (box_radius <= 0) return;
  for (int pass = 0; pass < 3; ++pass) {
    box_blur_pass(mask.values(), mask.width(), mask.height(), box_radius, true);
    box_blur_pass(mask.values(), mask.width(), mask.height(), box_radius, false);
  }
}

// —— Canvas ——

Canvas::Canvas(int physical_width, int physical_height, float device_scale)
    : physical_width_(physical_width > 0 ? physical_width : 0),
      physical_height_(physical_height > 0 ? physical_height : 0),
      scale_(device_scale > 0.0f ? device_scale : 1.0f),
      inverse_scale_(1.0f / (device_scale > 0.0f ? device_scale : 1.0f)),
      pixels_(static_cast<std::size_t>(physical_width_) * static_cast<std::size_t>(physical_height_),
              0U) {
  clip_stack_.push_back(
      ClipFrame{math::IntRect{0, 0, physical_width_, physical_height_}, nullptr, 0, 0});
}

auto Canvas::for_logical_size(int logical_width, int logical_height, float device_scale) -> Canvas {
  const float scale = device_scale > 0.0f ? device_scale : 1.0f;
  const auto physical_w = static_cast<int>(std::lround(static_cast<float>(logical_width) * scale));
  const auto physical_h = static_cast<int>(std::lround(static_cast<float>(logical_height) * scale));
  return Canvas(physical_w, physical_h, scale);
}

void Canvas::set_device_scale(float scale) noexcept {
  scale_ = scale > 0.0f ? scale : 1.0f;
  inverse_scale_ = 1.0f / scale_;
}

auto Canvas::to_physical(math::Rect rect) const noexcept -> math::IntRect {
  const math::Rect scaled = math::Rect{rect.x * scale_, rect.y * scale_, rect.width * scale_,
                                       rect.height * scale_};
  return scaled.round_out();
}

auto Canvas::to_physical(math::Point point) const noexcept -> math::Point {
  return math::Point{point.x * scale_, point.y * scale_};
}

auto Canvas::to_logical_point(float physical_x, float physical_y) const noexcept -> math::Point {
  return math::Point{physical_x * inverse_scale_, physical_y * inverse_scale_};
}

Canvas::~Canvas() = default;

Canvas::Canvas(Canvas&& other) noexcept
    : physical_width_(other.physical_width_),
      physical_height_(other.physical_height_),
      // **DPI 缩放必须一并搬运**：漏掉它会让"重建画布"（如运行时切 DPI）后的新画布
      // 尺寸是 2x 而换算仍按 1x，绘制结果只占左上 1/4——视觉上就是"内容缩在角落里"。
      scale_(other.scale_),
      inverse_scale_(other.inverse_scale_),
      pixels_(std::move(other.pixels_)),
      clip_stack_(std::move(other.clip_stack_)) {
  other.physical_width_ = 0;
  other.physical_height_ = 0;
}

auto Canvas::operator=(Canvas&& other) noexcept -> Canvas& {
  if (this != &other) {
    physical_width_ = other.physical_width_;
    physical_height_ = other.physical_height_;
    scale_ = other.scale_;                  // 见移动构造函数注释
    inverse_scale_ = other.inverse_scale_;
    pixels_ = std::move(other.pixels_);
    clip_stack_ = std::move(other.clip_stack_);
    other.physical_width_ = 0;
    other.physical_height_ = 0;
  }
  return *this;
}

void Canvas::clear(math::Color color) {
  const std::uint32_t value = math::premultiply(color);
  if (color.a == 255U) {
    std::ranges::fill(pixels_, value);
    return;
  }
  for (auto& pixel : pixels_) pixel = value;
}

auto Canvas::pixel_at(int x, int y) const -> math::Color {
  if (x < 0 || y < 0 || x >= physical_width_ || y >= physical_height_) return math::Color{0, 0, 0, 0};
  return math::unpremultiply(pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_) +
                                    static_cast<std::size_t>(x)]);
}

void Canvas::set_pixel(int x, int y, math::Color color) {
  if (x < 0 || y < 0 || x >= physical_width_ || y >= physical_height_) return;
  pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_) +
          static_cast<std::size_t>(x)] = math::premultiply(color);
}

auto Canvas::to_rgba8() const -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(physical_width_) * static_cast<std::size_t>(physical_height_) * 4U, 0U);
  std::size_t index = 0;
  for (const std::uint32_t pixel : pixels_) {
    const math::Color color = math::unpremultiply(pixel);
    out[index] = color.r;
    out[index + 1] = color.g;
    out[index + 2] = color.b;
    out[index + 3] = color.a;
    index += 4;
  }
  return out;
}

auto Canvas::content_bounds() const noexcept -> math::IntRect {
  int min_x = physical_width_;
  int min_y = physical_height_;
  int max_x = -1;
  int max_y = -1;
  for (int y = 0; y < physical_height_; ++y) {
    const auto* row =
        pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_);
    for (int x = 0; x < physical_width_; ++x) {
      if (channel(row[x], 0) != 0U) {
        if (x < min_x) min_x = x;
        if (y < min_y) min_y = y;
        if (x > max_x) max_x = x;
        if (y > max_y) max_y = y;
      }
    }
  }
  if (max_x < 0) return math::IntRect{};
  return math::IntRect{min_x, min_y, max_x - min_x + 1, max_y - min_y + 1};
}

auto Canvas::content_bounds_logical() const noexcept -> math::IntRect {
  const math::IntRect physical = content_bounds();
  if (physical.is_empty()) return physical;
  const float inv = inverse_scale_;
  const auto x0 = static_cast<int>(std::floor(static_cast<float>(physical.x) * inv));
  const auto y0 = static_cast<int>(std::floor(static_cast<float>(physical.y) * inv));
  const auto x1 = static_cast<int>(std::ceil(static_cast<float>(physical.right()) * inv));
  const auto y1 = static_cast<int>(std::ceil(static_cast<float>(physical.bottom()) * inv));
  return math::IntRect{x0, y0, x1 - x0, y1 - y0};
}

auto Canvas::pixel_at_point(math::Point point) const -> math::Color {
  const math::Point physical = to_physical(point);
  return pixel_at(static_cast<int>(std::floor(physical.x)),
                  static_cast<int>(std::floor(physical.y)));
}

auto Canvas::current_clip() const noexcept -> const ClipFrame& { return clip_stack_.back(); }

auto Canvas::clip_rect() const noexcept -> math::IntRect { return current_clip().rect; }

auto Canvas::has_mask_clip() const noexcept -> bool {
  for (const auto& frame : clip_stack_) {
    if (frame.mask != nullptr) return true;
  }
  return false;
}

auto Canvas::effective_alpha(int x, int y, float coverage) const noexcept -> float {
  float alpha = coverage;
  for (const auto& frame : clip_stack_) {
    if (frame.mask == nullptr) continue;
    alpha *= frame.mask->sample(static_cast<float>(x - frame.mask_origin_x) + 0.5f,
                                static_cast<float>(y - frame.mask_origin_y) + 0.5f);
    if (alpha <= kCoverageEpsilon) return 0.0f;
  }
  return alpha;
}

void Canvas::blend_span(int y, int x_begin, int x_end, math::Color color, float opacity,
                        BlendMode mode) {
  if (y < 0 || y >= physical_height_) return;
  const ClipFrame& clip = current_clip();
  if (y < clip.rect.y || y >= clip.rect.bottom()) return;
  const int begin = std::max(x_begin, clip.rect.x);
  const int end = std::min(x_end, clip.rect.right());
  if (begin >= end) return;

  auto* row = pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_);
  const auto count = static_cast<std::size_t>(end - begin);
  const bool plain_clip = !has_mask_clip();

  if (plain_clip && mode == BlendMode::Src && opacity >= 0.999f) {
    simd::fill_row(row + begin, count, math::premultiply(color));
    return;
  }
  if (plain_clip && mode == BlendMode::SrcOver) {
    const auto alpha_byte = static_cast<std::uint8_t>(math::clamp01(opacity) * 255.0f + 0.5f);
    simd::blend_row(row + begin, count, math::premultiply(color), alpha_byte);
    return;
  }
  for (int x = begin; x < end; ++x) {
    const float alpha = effective_alpha(x, y, opacity);
    if (alpha <= kCoverageEpsilon) continue;
    row[x] = blend_pixel(row[x], color, alpha, mode);
  }
}

void Canvas::blend_coverage_row(int y, int x_begin, std::span<const float> coverage,
                                const Paint& paint, float opacity, BlendMode blend) {
  if (y < 0 || y >= physical_height_) return;
  const ClipFrame& clip = current_clip();
  if (y < clip.rect.y || y >= clip.rect.bottom()) return;
  const int begin = std::max(x_begin, clip.rect.x);
  const int end = std::min(x_begin + static_cast<int>(coverage.size()), clip.rect.right());
  if (begin >= end) return;
  auto* row = pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_);
  for (int x = begin; x < end; ++x) {
    // 覆盖率为**带符号**累加（非零环绕规则：顺时针 +、逆时针 −，孔洞处相互抵消为 0），
    // 因此这里取绝对值作为不透明度——直接使用原值会把逆时针轮廓整片丢弃。
    float alpha = std::abs(coverage[static_cast<std::size_t>(x - x_begin)]);
    if (alpha <= kCoverageEpsilon) continue;
    alpha = effective_alpha(x, y, alpha * opacity);
    if (alpha <= kCoverageEpsilon) continue;
    const math::Color color =
        paint.sample(to_logical_point(static_cast<float>(x) + 0.5f,
                                     static_cast<float>(y) + 0.5f));
    if (color.a == 0U) continue;
    row[x] = blend_pixel(row[x], color, alpha, blend);
  }
}

void Canvas::fill_path(const Path& path, const Paint& paint, DrawOptions options) {
  if (scale_ == 1.0f) {
    detail::fill_path_aa(*this, path, paint, options);
    return;
  }
  detail::fill_path_aa(*this, path.scaled(scale_), paint, options);
}

void Canvas::fill_rect(math::Rect rect, const Paint& paint, float radius, DrawOptions options) {
  if (rect.is_empty() || paint.color().a == 0U) return;
  // DPI：逻辑坐标 → 物理像素（scale=1 时零开销）
  const math::Rect logical_rect = rect;
  if (scale_ != 1.0f) {
    rect = math::Rect{rect.x * scale_, rect.y * scale_, rect.width * scale_, rect.height * scale_};
    radius *= scale_;
  }
  const math::IntRect area = rect.round_out();
  const math::IntRect clipped = area.intersect(clip_rect());
  if (clipped.is_empty()) return;

  const bool aligned = rect.x == std::floor(rect.x) && rect.y == std::floor(rect.y) &&
                       rect.right() == std::floor(rect.right()) &&
                       rect.bottom() == std::floor(rect.bottom());
  const bool simple = paint.is_solid() && options.antialias && radius <= 0.0f && aligned &&
                      options.blend == BlendMode::SrcOver;
  if (simple) {
    const auto left = static_cast<int>(rect.x);
    const auto right = static_cast<int>(rect.right());
    const auto top = static_cast<int>(rect.y);
    const auto bottom = static_cast<int>(rect.bottom());
    for (int y = std::max(top, clipped.y); y < std::min(bottom, clipped.bottom()); ++y) {
      blend_span(y, left, right, paint.color(), options.opacity, options.blend);
    }
    return;
  }

  Path path;
  if (radius > 0.0f) {
    path.add_rounded_rect(rect, radius);
  } else {
    path.add_rect(rect);
  }
  detail::fill_path_aa(*this, path, paint, options);
  (void)logical_rect;
}

void Canvas::fill_circle(math::Point center, float radius, const Paint& paint,
                         DrawOptions options) {
  if (radius <= 0.0f) return;
  if (scale_ != 1.0f) {
    center = to_physical(center);
    radius *= scale_;
  }
  Path path;
  path.add_circle(center, radius);
  detail::fill_path_aa(*this, path, paint, options);
}

void Canvas::stroke_path(const Path& path, const Paint& paint, float width, DrawOptions options) {
  if (width <= 0.0f || path.is_empty()) return;
  // DPI：描边宽度同比例放大，1px 发丝线在 2x 屏上是 2 物理像素（视觉等宽且更锐利）
  const Path& source = path;
  const float physical_width_value = width * scale_;
  Path scaled_path = scale_ == 1.0f ? Path{} : source.scaled(scale_);
  const Path& effective = scale_ == 1.0f ? source : scaled_path;
  Path outline = detail::stroke_to_path(effective, physical_width_value,
                                        options.antialias ? 0.25f : 0.5f);
  detail::fill_path_aa(*this, outline, paint, options);
}

void Canvas::draw_shadow(math::Rect rect, float radius, float blur, math::Color color,
                         math::Point offset, DrawOptions options) {
  if (rect.is_empty() || color.a == 0U) return;
  if (scale_ != 1.0f) {
    rect = math::Rect{rect.x * scale_, rect.y * scale_, rect.width * scale_, rect.height * scale_};
    radius *= scale_;
    blur *= scale_;
    offset = math::Point{offset.x * scale_, offset.y * scale_};
  }
  const float padding = blur * 2.0f + 2.0f;
  const math::Rect region = rect.offset(offset.x, offset.y).inflate(padding);
  const math::IntRect area = region.round_out().intersect(clip_rect());
  if (area.is_empty()) return;

  Mask mask(area.width, area.height);
  const math::Rect local = rect.offset(offset.x - static_cast<float>(area.x),
                                       offset.y - static_cast<float>(area.y));
  detail::rasterize_mask(mask, make_rounded_rect(local, radius), 0.0f, 0.0f);
  blur_mask(mask, blur);

  auto* base = pixels_.data();
  for (int y = area.y; y < area.bottom(); ++y) {
    auto* row = base + static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_);
    for (int x = area.x; x < area.right(); ++x) {
      const float mask_alpha = mask.sample(static_cast<float>(x - area.x) + 0.5f,
                                           static_cast<float>(y - area.y) + 0.5f);
      if (mask_alpha <= kCoverageEpsilon) continue;
      const float alpha = effective_alpha(x, y, mask_alpha * options.opacity);
      if (alpha <= kCoverageEpsilon) continue;
      row[x] = blend_pixel(row[x], color, alpha, options.blend);
    }
  }
}

void Canvas::draw_canvas(const Canvas& source, math::Rect destination, DrawOptions options) {
  if (destination.is_empty() || source.physical_width_ <= 0 || source.physical_height_ <= 0) return;
  // destination 为逻辑坐标 → 物理像素；采样源为其物理缓冲
  const math::IntRect area = to_physical(destination).intersect(clip_rect());
  if (area.is_empty()) return;
  const math::Rect physical_destination =
      scale_ == 1.0f
          ? destination
          : math::Rect{destination.x * scale_, destination.y * scale_, destination.width * scale_,
                       destination.height * scale_};
  const float scale_x = static_cast<float>(source.physical_width_) / physical_destination.width;
  const float scale_y = static_cast<float>(source.physical_height_) / physical_destination.height;

  for (int y = area.y; y < area.bottom(); ++y) {
    auto* row = pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_);
    const float source_y =
        (static_cast<float>(y) + 0.5f - physical_destination.y) * scale_y - 0.5f;
    for (int x = area.x; x < area.right(); ++x) {
      const float source_x =
          (static_cast<float>(x) + 0.5f - physical_destination.x) * scale_x - 0.5f;
      const float clamped_x =
          math::clampf(source_x, 0.0f, static_cast<float>(source.physical_width_ - 1));
      const float clamped_y =
          math::clampf(source_y, 0.0f, static_cast<float>(source.physical_height_ - 1));
      const auto x0 = static_cast<int>(clamped_x);
      const auto y0 = static_cast<int>(clamped_y);
      const int x1 = x0 + 1 < source.physical_width_ ? x0 + 1 : x0;
      const int y1 = y0 + 1 < source.physical_height_ ? y0 + 1 : y0;
      const float fx = clamped_x - static_cast<float>(x0);
      const float fy = clamped_y - static_cast<float>(y0);
      const auto sample = [&source](int sx, int sy) noexcept -> std::uint32_t {
        return source.pixels_[static_cast<std::size_t>(sy) *
                                  static_cast<std::size_t>(source.physical_width_) +
                              static_cast<std::size_t>(sx)];
      };
      const auto interpolate = [fx, fy](std::uint32_t a, std::uint32_t b, std::uint32_t cc,
                                        std::uint32_t d, unsigned shift) noexcept -> std::uint32_t {
        const auto top = static_cast<float>((a >> shift) & 0xFFU) +
                         (static_cast<float>((b >> shift) & 0xFFU) -
                          static_cast<float>((a >> shift) & 0xFFU)) * fx;
        const auto bottom = static_cast<float>((cc >> shift) & 0xFFU) +
                            (static_cast<float>((d >> shift) & 0xFFU) -
                             static_cast<float>((cc >> shift) & 0xFFU)) * fx;
        return static_cast<std::uint32_t>(top + (bottom - top) * fy + 0.5f);
      };
      const std::uint32_t p00 = sample(x0, y0);
      const std::uint32_t p10 = sample(x1, y0);
      const std::uint32_t p01 = sample(x0, y1);
      const std::uint32_t p11 = sample(x1, y1);
      std::uint32_t blended =
          pack(interpolate(p00, p10, p01, p11, 24), interpolate(p00, p10, p01, p11, 16),
               interpolate(p00, p10, p01, p11, 8), interpolate(p00, p10, p01, p11, 0));
      if (options.opacity < 0.999f) {
        const auto alpha =
            static_cast<std::uint32_t>(math::clamp01(options.opacity) * 255.0f + 0.5f);
        blended = scale_premul(blended, alpha);
      }
      const float coverage = static_cast<float>(channel(blended, 0)) / 255.0f;
      if (coverage <= kCoverageEpsilon) continue;
      row[x] = over_premul(row[x], blended);
    }
  }
}

void Canvas::draw_canvas_at(const Canvas& source, int x, int y, DrawOptions options) {
  draw_canvas(source,
              math::Rect{static_cast<float>(x), static_cast<float>(y),
                         static_cast<float>(source.width()), static_cast<float>(source.height())},
              options);
}

void Canvas::push_clip_rect(math::Rect rect) {
  ClipFrame frame = current_clip();
  frame.rect = frame.rect.intersect(to_physical(rect));
  frame.mask_origin_x = frame.rect.x;
  frame.mask_origin_y = frame.rect.y;
  clip_stack_.push_back(frame);
}

void Canvas::push_clip_rounded_rect(math::Rect rect, float radius) {
  // 入参是**逻辑坐标**：换算到物理后再构造路径（否则 2x 屏上裁剪区域只覆盖左上 1/4，
  // 子项会被整片裁掉——表现为"卡片里的内容凭空消失"）。
  const math::Rect physical = math::Rect{rect.x * scale_, rect.y * scale_, rect.width * scale_,
                                         rect.height * scale_};
  Path path;
  path.add_rounded_rect(physical, radius * scale_);
  push_clip_path(path);
}

void Canvas::push_clip_path(const Path& path) {
  // 与 `push_clip_rect` 一致：**路径按物理像素解释**（raster 层其余 API 处理的是物理像素；
  // 逻辑坐标的换算在 UI 侧进入画布接口时完成）。调用方若持有逻辑坐标路径，请先 `scaled(dpr)`。
  const math::Rect bounds = path.flattened_bounds(0.25f);
  const math::IntRect area = bounds.round_out().inflate(1.0f).intersect(clip_rect());
  if (area.is_empty()) {
    ClipFrame frame = current_clip();
    frame.rect = math::IntRect{};
    clip_stack_.push_back(frame);
    return;
  }
  auto mask = std::make_shared<Mask>(area.width, area.height);
  detail::rasterize_mask(*mask, path, static_cast<float>(area.x), static_cast<float>(area.y));
  ClipFrame frame = current_clip();
  frame.rect = area;
  frame.mask = std::move(mask);
  frame.mask_origin_x = area.x;
  frame.mask_origin_y = area.y;
  clip_stack_.push_back(std::move(frame));
}

void Canvas::pop_clip() {
  if (clip_stack_.size() > 1) clip_stack_.pop_back();
}

}  // namespace st::raster
