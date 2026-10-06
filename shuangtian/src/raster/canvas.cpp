#include "st/raster/canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "rasterize_internal.hpp"
#include "st/core/fs.hpp"
#include "st/core/time.hpp"
#include "st/raster/simd.hpp"

namespace st::raster {

// —— PaintProfiler ——

auto paint_op_name(PaintOp op) -> std::string_view {
  switch (op) {
    case PaintOp::Clear: return "clear";
    case PaintOp::FillRect: return "fill_rect";
    case PaintOp::FillRoundRect: return "round_rect";
    case PaintOp::FillPath: return "fill_path";
    case PaintOp::Stroke: return "stroke";
    case PaintOp::Shadow: return "shadow";
    case PaintOp::Image: return "image";
    case PaintOp::ClipMask: return "clip_mask";
    case PaintOp::Text: return "text";
    case PaintOp::Count: break;
  }
  return "unknown";
}

void PaintProfiler::add(PaintOp op, double ms, std::uint64_t pixels) {
  if (op == PaintOp::Count) return;
  PaintOpStat& stat = ops[static_cast<std::size_t>(op)];
  ++stat.calls;
  stat.pixels += pixels;
  stat.ms += ms;
}

auto PaintProfiler::op(PaintOp which) const noexcept -> const PaintOpStat& {
  return ops[static_cast<std::size_t>(which)];
}

auto PaintProfiler::total_ms() const noexcept -> double {
  double total = 0.0;
  for (const auto& stat : ops) total += stat.ms;
  return total;
}

void PaintProfiler::clear() noexcept { ops.fill(PaintOpStat{}); }

namespace {

inline constexpr float kCoverageEpsilon = 0.0005f;

/// 原语计时作用域（仅当画布挂了剖析器时记账）。
struct OpScope {
  Canvas& canvas;
  PaintOp op;
  std::int64_t start_ns;
  std::uint64_t pixels;

  OpScope(Canvas& target, PaintOp which) noexcept
      : canvas(target), op(which), start_ns(target.profiler() != nullptr ? time::now_ns() : 0),
        pixels(0) {}
  OpScope(const OpScope&) = delete;
  auto operator=(const OpScope&) -> OpScope& = delete;
  ~OpScope() {
    if (start_ns == 0) return;
    canvas.add_profile(op, static_cast<double>(time::now_ns() - start_ns) / 1'000'000.0, pixels);
  }
  void set_pixels(std::uint64_t value) noexcept { pixels = value; }
};

/// 预乘像素分量解包（位布局：R<<24 | G<<16 | B<<8 | A）。
[[nodiscard]] constexpr auto channel(std::uint32_t pixel, unsigned shift) noexcept -> std::uint32_t {
  return (pixel >> shift) & 0xFFU;
}

[[nodiscard]] constexpr auto pack(std::uint32_t red, std::uint32_t green, std::uint32_t blue,
                                  std::uint32_t alpha) noexcept -> std::uint32_t {
  return (red << 24U) | (green << 16U) | (blue << 8U) | alpha;
}

/// `value / 255` 的乘法-移位形式（对 `value < 2^16` 与整数除法**逐位一致**）。
///
/// 为什么必须换掉 `/255`：x86 的整数除法是几十个周期，而逐像素混合里最多要做 4 次
/// （`over_premul` 与 `scale_premul` 各 4 个通道）。阴影与文字这类“大面积逐像素混合”
/// 的开销几乎全在这几条除法上（实测：换成乘法-移位后阴影一帧从 10ms 降到 4ms）。
/// 魔法常数：`x / 255 == (x * 0x8081) >> 23`（仅对 `x < 2^16` 成立，本用途值域满足）。
[[nodiscard]] constexpr auto fast_div255(std::uint32_t value) noexcept -> std::uint32_t {
  return (value * 0x8081U) >> 23U;
}

/// 预乘源覆盖（源已按 alpha 缩放）。
[[nodiscard]] constexpr auto over_premul(std::uint32_t dst, std::uint32_t src) noexcept
    -> std::uint32_t {
  const std::uint32_t sa = channel(src, 0);
  if (sa == 0U) return dst;
  const std::uint32_t inverse = 255U - sa;
  const auto mix = [inverse](std::uint32_t source, std::uint32_t destination) constexpr noexcept {
    return source + fast_div255(destination * inverse + 127U);
  };
  return pack(mix(channel(src, 24), channel(dst, 24)), mix(channel(src, 16), channel(dst, 16)),
              mix(channel(src, 8), channel(dst, 8)), mix(sa, channel(dst, 0)));
}

/// 按覆盖率缩放预乘源。
[[nodiscard]] constexpr auto scale_premul(std::uint32_t src, std::uint32_t alpha) noexcept
    -> std::uint32_t {
  const auto scale = [alpha](std::uint32_t value) constexpr noexcept -> std::uint32_t {
    return fast_div255(value * alpha + 127U);
  };
  return pack(scale(channel(src, 24)), scale(channel(src, 16)), scale(channel(src, 8)),
              scale(channel(src, 0)));
}

/// 逐通道 α 的预乘 src-over：`out_c = S_c·α_c + D_c·(1 - a_s·α_c)`。
///
/// 亚像素渲染的全部内容就是这条式子：**目标衰减也按通道**。若把它写成
/// `S_c·α_c + D_c·(1 - ā)`（硬件混合只能给一个标量 α），黑字压白底时 `S_c = 0`，
/// 结果退化成 `D_c·(1-ā)`——**彩边一个都不剩**，等于白做。
/// GPU 侧因此必须走两遍混合（先把目标按 `1-α_c` 衰减，再加性加回源项）。
[[nodiscard]] constexpr auto over_premul_lcd(std::uint32_t dst, std::uint32_t src,
                                             std::uint32_t a_r, std::uint32_t a_g,
                                             std::uint32_t a_b) noexcept -> std::uint32_t {
  const std::uint32_t source_alpha = channel(src, 0);
  const auto blend = [source_alpha](std::uint32_t source, std::uint32_t destination,
                                    std::uint32_t alpha) constexpr noexcept {
    // 源项按 α_c 缩放；目标衰减按 a_s·α_c（源自身不透明时二者相同）
    const std::uint32_t attenuation = fast_div255(source_alpha * alpha + 127U);
    return fast_div255(source * alpha + 127U) +
           fast_div255(destination * (255U - attenuation) + 127U);
  };
  // α 通道是**标量**，无法逐通道：取三通道均值（不透明画布上与逐通道等价）
  const std::uint32_t a_a = (a_r + a_g + a_b + 1U) / 3U;
  return pack(blend(channel(src, 24), channel(dst, 24), a_r),
              blend(channel(src, 16), channel(dst, 16), a_g),
              blend(channel(src, 8), channel(dst, 8), a_b),
              blend(source_alpha, channel(dst, 0), a_a));
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
[[nodiscard]] auto blend_pixel_premul(std::uint32_t dst, std::uint32_t source_premul, float coverage,
                                      BlendMode mode) noexcept -> std::uint32_t {
  const float clamped = coverage <= 1.0f ? coverage : 1.0f;
  const auto alpha_byte = static_cast<std::uint32_t>(clamped * 255.0f + 0.5f);
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
  const math::Color source = math::unpremultiply(source_premul);
  const bool has_backdrop = backdrop.a != 0U;
  std::uint32_t blended = 0;
  if (has_backdrop) {
    const auto to_byte = [](std::uint8_t value) noexcept -> std::uint32_t {
      return static_cast<std::uint32_t>(value);
    };
    blended = pack(blend_channel(mode, to_byte(backdrop.r), to_byte(source.r)),
                   blend_channel(mode, to_byte(backdrop.g), to_byte(source.g)),
                   blend_channel(mode, to_byte(backdrop.b), to_byte(source.b)), 255U);
    blended = math::premultiply(math::unpremultiply(blended).with_alpha(source.a));
  } else {
    blended = scale_premul(source_premul, 255U);
  }
  return over_premul(dst, scale_premul(blended, alpha_byte));
}

/// 直通颜色的版本（逐像素采样画笔的路径用：颜色每像素都不同）。
[[nodiscard]] auto blend_pixel(std::uint32_t dst, math::Color color, float coverage,
                               BlendMode mode) noexcept -> std::uint32_t {
  return blend_pixel_premul(dst, math::premultiply(color), coverage, mode);
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
  const float fx = clamped_x - static_cast<float>(x0);
  const float fy = clamped_y - static_cast<float>(y0);
  // 像素中心对齐采样（调用方一律传 `整数 + 0.5`）→ 双线性退化为最近邻。
  // 这是遮罩裁剪/阴影的**常态**，而双线性在这里是 4 次读 + 6 次浮点运算。
  if (fx <= 0.0f && fy <= 0.0f) {
    return static_cast<float>(values_[static_cast<std::size_t>(y0) *
                                       static_cast<std::size_t>(width_) +
                                       static_cast<std::size_t>(x0)]) /
           255.0f;
  }
  const int x1 = x0 + 1 < width_ ? x0 + 1 : x0;
  const int y1 = y0 + 1 < height_ ? y0 + 1 : y0;
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

auto Canvas::resize(int physical_width, int physical_height) -> Status {
  const int width = physical_width > 0 ? physical_width : 0;
  const int height = physical_height > 0 ? physical_height : 0;
  if (width == 0 || height == 0) {
    return unexpected(ErrorCode::Invalid, "画布尺寸必须为正");
  }
  if (width == physical_width_ && height == physical_height_) return ok();
  physical_width_ = width;
  physical_height_ = height;
  pixels_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
  // 裁剪栈重置：旧栈顶的矩形是按旧尺寸算的，留着会让新画布的内容被裁到"旧的那块"。
  clip_stack_.clear();
  clip_stack_.push_back(ClipFrame{math::IntRect{0, 0, width, height}, nullptr, 0, 0});
  // 阴影遮罩缓存**故意保留**：见头文件里的说明（它与画布尺寸无关）。
  return ok();
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
      clip_stack_(std::move(other.clip_stack_)),
      // 阴影遮罩缓存一并搬运：漏掉虽不会立刻出错，但会让"移动后的新画布"从零重建遮罩，
      // 而旧画布的缓存会随它一起销毁（白白丢掉本可复用的模糊结果）。
      shadow_masks_(std::move(other.shadow_masks_)),
      profiler_(other.profiler_) {
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
    shadow_masks_ = std::move(other.shadow_masks_);  // 见移动构造函数注释
    profiler_ = other.profiler_;
    other.physical_width_ = 0;
    other.physical_height_ = 0;
  }
  return *this;
}

void Canvas::clear(math::Color color) {
  OpScope scope(*this, PaintOp::Clear);
  scope.set_pixels(static_cast<std::uint64_t>(physical_width_) *
                   static_cast<std::uint64_t>(physical_height_));
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

void Canvas::blend_coverage_bitmap(int x, int y, std::span<const float> coverage, int width,
                                   int height, const Paint& paint, float opacity,
                                   BlendMode blend, std::uint64_t cache_key,
                                   CoverageFormat format) {
  // 软件路径逐行混合，不需要稳定身份（身份只服务 GPU 那边的纹理缓存）。
  (void)cache_key;
  if (width <= 0 || height <= 0 || opacity <= 0.0f) return;
  const std::size_t channels = format == CoverageFormat::Lcd ? 3U : 1U;
  const std::size_t stride = static_cast<std::size_t>(width) * channels;
  const std::size_t expected = static_cast<std::size_t>(height) * stride;
  if (coverage.size() < expected) return;
  // 软件实现就是“逐行走行混合”——这正是 `blend_coverage_row` 的用途；
  // GPU 实现则把这张覆盖率图传成纹理再画一个四边形（两边语义相同，做法不同）。
  //
  // **本函数内部不记账**：归到哪一类原语取决于调用方（字形→`Text`、遮罩→`Image`），
  // 在这里自己记一笔会让剖析出现双重归属。
  for (int row = 0; row < height; ++row) {
    const std::span<const float> line(coverage.data() + static_cast<std::size_t>(row) * stride,
                                      stride);
    if (format == CoverageFormat::Lcd) {
      blend_coverage_row_subpixel(y + row, x, line, paint, opacity, blend);
    } else {
      blend_coverage_row(y + row, x, line, paint, opacity, blend);
    }
  }
}

void Canvas::blend_coverage_row(int y, int x_begin, std::span<const float> coverage,
                                const Paint& paint, float opacity, BlendMode blend) {  if (y < 0 || y >= physical_height_) return;
  const ClipFrame& clip = current_clip();
  if (y < clip.rect.y || y >= clip.rect.bottom()) return;
  const int begin = std::max(x_begin, clip.rect.x);
  const int end = std::min(x_begin + static_cast<int>(coverage.size()), clip.rect.right());
  if (begin >= end) return;
  auto* row = pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_);
  // **裁剪栈遍历是逐行属性，不是逐像素属性**：遮罩裁剪只在圆角裁剪/阴影路径里出现，
  // 常规界面（卡片/表格行/面板）全是纯矩形裁剪——每像素遍历一次裁剪栈纯属冤枉。
  const bool masked = has_mask_clip();
  // 覆盖率数组按**相对 x_begin 的下标**访问（不构造“数组之前”的指针：那是未定义行为，
  // 在 MSVC 的优化器下会真的出事）。
  const auto coverage_at = [&coverage, x_begin](int x) -> float {
    return coverage[static_cast<std::size_t>(x - x_begin)];
  };

  if (paint.is_solid()) {
    const math::Color color = paint.color();
    if (color.a == 0U) return;
    // 预乘颜色**一次**算好：原实现在逐像素里做 3 次整数除法。
    const std::uint32_t premul = math::premultiply(color);
    if (!masked && blend == BlendMode::SrcOver) {
      const auto full_alpha = static_cast<std::uint8_t>(math::clamp01(opacity) * 255.0f + 0.5f);
      int x = begin;
      while (x < end) {
        // 覆盖率满格（≥ 0.999）的连续段——圆角矩形的内部、表格行、面板——整段交给 SIMD，
        // 跳过整串逐像素浮点运算；这也是大面填充能快一个量级的关键。
        if (coverage_at(x) >= 0.999f) {
          const int run_begin = x;
          while (x < end && coverage_at(x) >= 0.999f) ++x;
          simd::blend_row(row + run_begin, static_cast<std::size_t>(x - run_begin), premul,
                          full_alpha);
          continue;
        }
        const float alpha = std::abs(coverage_at(x)) * opacity;
        if (alpha > kCoverageEpsilon) {
          const auto alpha_byte = static_cast<std::uint32_t>(alpha * 255.0f + 0.5f);
          row[x] = over_premul(row[x], scale_premul(premul, alpha_byte));
        }
        ++x;
      }
      return;
    }
    // 带遮罩裁剪或非常规混合模式：走通用路径（预乘仍只算一次）
    for (int x = begin; x < end; ++x) {
      float alpha = std::abs(coverage_at(x));
      if (alpha <= kCoverageEpsilon) continue;
      alpha *= opacity;
      if (masked) {
        alpha = effective_alpha(x, y, alpha);
        if (alpha <= kCoverageEpsilon) continue;
      }
      row[x] = blend_pixel_premul(row[x], premul, alpha, blend);
    }
    return;
  }

  // 渐变画笔：颜色仍须逐像素采样，但坐标换算改为**递推**（每像素一次加法，不再是两次乘法）
  const float step = inverse_scale_;
  float logical_x = (static_cast<float>(begin) + 0.5f) * inverse_scale_;
  const float logical_y = (static_cast<float>(y) + 0.5f) * inverse_scale_;
  for (int x = begin; x < end; ++x, logical_x += step) {
    float alpha = std::abs(coverage_at(x));
    if (alpha <= kCoverageEpsilon) continue;
    alpha *= opacity;
    if (masked) {
      alpha = effective_alpha(x, y, alpha);
      if (alpha <= kCoverageEpsilon) continue;
    }
    const math::Color color = paint.sample(math::Point{logical_x, logical_y});
    if (color.a == 0U) continue;
    row[x] = blend_pixel_premul(row[x], math::premultiply(color), alpha, blend);
  }
}

void Canvas::blend_coverage_row_lcd(int y, int x_begin, std::span<const float> coverage,
                                    const Paint& paint, float opacity, BlendMode blend) {
  // 亚像素混合的既有实现（算法未动，只是换个名字对上 `Surface` 的逐行原语）。
  blend_coverage_row_subpixel(y, x_begin, coverage, paint, opacity, blend);
}

void Canvas::blend_coverage_row_subpixel(int y, int x_begin, std::span<const float> coverage,
                                         const Paint& paint, float opacity, BlendMode blend) {
  if (y < 0 || y >= physical_height_) return;
  const ClipFrame& clip = current_clip();
  if (y < clip.rect.y || y >= clip.rect.bottom()) return;
  const int pixels = static_cast<int>(coverage.size() / 3U);
  const int begin = std::max(x_begin, clip.rect.x);
  const int end = std::min(x_begin + pixels, clip.rect.right());
  if (begin >= end) return;
  auto* row = pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_);
  const bool masked = has_mask_clip();
  const auto coverage_at = [&coverage, x_begin](int x, std::size_t channel) -> float {
    return coverage[static_cast<std::size_t>(x - x_begin) * 3U + channel];
  };

  if (blend != BlendMode::SrcOver) {
    // 非常规混合模式（Multiply/Overlay/…）的定义建立在**标量**覆盖率上，没有逐通道版本：
    // 退化成三通道均值走单通道路径——宁可少一点彩边，不可错一点颜色。
    std::vector<float> averaged(static_cast<std::size_t>(end - begin));
    for (int x = begin; x < end; ++x) {
      averaged[static_cast<std::size_t>(x - begin)] =
          (std::abs(coverage_at(x, 0U)) + std::abs(coverage_at(x, 1U)) +
           std::abs(coverage_at(x, 2U))) /
          3.0f;
    }
    blend_coverage_row(y, begin, averaged, paint, opacity, blend);
    return;
  }

  // 逐通道 α（含 opacity 与遮罩裁剪）
  const auto alpha_of = [&](int x, std::size_t channel) -> std::uint32_t {
    float alpha = std::abs(coverage_at(x, channel)) * opacity;
    if (masked) alpha = effective_alpha(x, y, alpha);
    if (alpha <= 0.0f) return 0U;
    return static_cast<std::uint32_t>(math::clamp01(alpha) * 255.0f + 0.5f);
  };

  if (paint.is_solid()) {
    const math::Color color = paint.color();
    if (color.a == 0U) return;
    const std::uint32_t premul = math::premultiply(color);
    for (int x = begin; x < end; ++x) {
      const std::uint32_t a_r = alpha_of(x, 0U);
      const std::uint32_t a_g = alpha_of(x, 1U);
      const std::uint32_t a_b = alpha_of(x, 2U);
      // 三通道全零 = 这个像素一点墨都没有（空白区占绝大多数）——直接跳过
      if (a_r == 0U && a_g == 0U && a_b == 0U) continue;
      row[x] = over_premul_lcd(row[x], premul, a_r, a_g, a_b);
    }
    return;
  }

  // 渐变画笔：颜色仍须逐像素采样（坐标递推与单通道版同一口径）
  const float step = inverse_scale_;
  float logical_x = (static_cast<float>(begin) + 0.5f) * inverse_scale_;
  const float logical_y = (static_cast<float>(y) + 0.5f) * inverse_scale_;
  for (int x = begin; x < end; ++x, logical_x += step) {
    const std::uint32_t a_r = alpha_of(x, 0U);
    const std::uint32_t a_g = alpha_of(x, 1U);
    const std::uint32_t a_b = alpha_of(x, 2U);
    if (a_r == 0U && a_g == 0U && a_b == 0U) continue;
    const math::Color color = paint.sample(math::Point{logical_x, logical_y});
    if (color.a == 0U) continue;
    row[x] = over_premul_lcd(row[x], math::premultiply(color), a_r, a_g, a_b);
  }
}

void Canvas::blend_coverage_runs(int y, std::span<const CoverageRun> runs, const Paint& paint,
                                float opacity, BlendMode blend) {
  if (y < 0 || y >= physical_height_ || runs.empty()) return;
  const ClipFrame& clip = current_clip();
  if (y < clip.rect.y || y >= clip.rect.bottom()) return;
  const float clip_left = static_cast<float>(clip.rect.x);
  const float clip_right = static_cast<float>(clip.rect.right());
  auto* row = pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_);
  const bool masked = has_mask_clip();
  const bool solid = paint.is_solid();
  if (solid && paint.color().a == 0U) return;
  const std::uint32_t premul = solid ? math::premultiply(paint.color()) : 0U;
  const Gradient* const gradient = solid ? nullptr : paint.gradient();
  // 行中心对应的逻辑 y（行内常量；渐变快路径与回退路径共用）
  const float logical_y = (static_cast<float>(y) + 0.5f) * inverse_scale_;

  // 单个像素按覆盖率混合（端点像素与“带遮罩/非常规混合模式”用）
  const auto blend_one = [&](int x, float alpha, math::Color color) {
    if (alpha <= kCoverageEpsilon) return;
    alpha *= opacity;
    if (masked) {
      alpha = effective_alpha(x, y, alpha);
      if (alpha <= kCoverageEpsilon) return;
    }
    if (solid) {
      row[x] = blend_pixel_premul(row[x], premul, alpha, blend);
      return;
    }
    if (color.a == 0U) return;
    row[x] = blend_pixel_premul(row[x], math::premultiply(color), alpha, blend);
  };

  for (const auto& run : runs) {
    // 覆盖率为**带符号**累加（非零环绕规则：顺时针 +、逆时针 −），取绝对值作为不透明度
    const float weight = std::abs(run.weight);
    if (weight <= kCoverageEpsilon) continue;
    const float a = std::max(run.x0, clip_left);
    const float b = std::min(run.x1, clip_right);
    if (b <= a) continue;

    // 与 [a,b) 相交的像素区间：完全覆盖的整段走恒定 alpha（可 SIMD），端点像素单独算
    const int first_pixel = static_cast<int>(std::floor(a));
    const int last_pixel = static_cast<int>(std::ceil(b));
    const int full_begin = first_pixel + (static_cast<float>(first_pixel) < a ? 1 : 0);
    const int full_end = last_pixel - (static_cast<float>(last_pixel) > b ? 1 : 0);
    const auto cover_of = [&](int x) -> float {
      const float left = std::max(a, static_cast<float>(x));
      const float right = std::min(b, static_cast<float>(x) + 1.0f);
      return right > left ? (right - left) * weight : 0.0f;
    };

    if (solid) {
      if (!masked && blend == BlendMode::SrcOver) {
        if (first_pixel < full_begin) blend_one(first_pixel, cover_of(first_pixel), {});
        if (full_end > full_begin) {
          const auto alpha_byte =
              static_cast<std::uint8_t>(math::clamp01(weight * opacity) * 255.0f + 0.5f);
          simd::blend_row(row + full_begin, static_cast<std::size_t>(full_end - full_begin), premul,
                          alpha_byte);
        }
        // 尾部端点**只在它的右边界超出区间时**才单独处理：
        // 写成“无条件把 last_pixel-1 再画一次”会让像素对齐的区间把末尾像素**混合两次**
        // （表现为焦点环/描边一端明显更深——实测被单像素比对的用例抓出来）。
        if (static_cast<float>(last_pixel) > b && last_pixel - 1 >= full_begin) {
          blend_one(last_pixel - 1, cover_of(last_pixel - 1), {});
        }
        continue;
      }
      for (int x = first_pixel; x < last_pixel; ++x) blend_one(x, cover_of(x), {});
      continue;
    }

    // 渐变画笔：颜色逐像素采样（**只碰真的相交的像素**）。
    //
    // 优化（bench：此原语曾达清屏的 ~160×，是最贵的一块）：
    // ① **竖直线性渐变**（dx≈0，主题里最常见）：整行颜色恒定——逐像素采样退化为
    //    每行一次，完全覆盖段直接走 SIMD 整行混合（与纯色同一套 `blend_row`）；
    // ② 其余线性渐变：位置参数 `t` 沿行**递推**（每像素一次加法），
    //    不再逐像素做点积 + 除法；
    // ③ 径向/扫掠：几何非线性，保持逐像素采样（只在真的相交的像素上）。
    if (gradient != nullptr) {
      const math::Point from = gradient->start();
      const math::Point to = gradient->end();
      const float gdx = to.x - from.x;
      const float gdy = to.y - from.y;
      const float length_squared = gdx * gdx + gdy * gdy;
      const bool vertical = std::abs(gdx) <= 0.0002f * std::max(1.0f, std::abs(gdy));
      if (vertical && length_squared > 0.0001f) {
        // t 只随 y 变化：整行一个颜色（直接从几何算，不必构造点再投影）
        const float row_position = (logical_y - from.y) * gdy / length_squared;
        const math::Color row_color = gradient->sample_position(row_position);
        const std::uint32_t row_premul = math::premultiply(row_color);
        if (!masked && blend == BlendMode::SrcOver && row_color.a != 0U) {
          if (first_pixel < full_begin) blend_one(first_pixel, cover_of(first_pixel), row_color);
          if (full_end > full_begin) {
            const auto alpha_byte =
                static_cast<std::uint8_t>(math::clamp01(weight * opacity) * 255.0f + 0.5f);
            simd::blend_row(row + full_begin, static_cast<std::size_t>(full_end - full_begin),
                            row_premul, alpha_byte);
          }
          if (static_cast<float>(last_pixel) > b && last_pixel - 1 >= full_begin) {
            blend_one(last_pixel - 1, cover_of(last_pixel - 1), row_color);
          }
          continue;
        }
        for (int x = first_pixel; x < last_pixel; ++x) blend_one(x, cover_of(x), row_color);
        continue;
      }
      if (gradient->kind() == Gradient::Kind::Linear && length_squared > 0.0001f) {
        // 倾斜线性：t 沿行递推（t += dx·step/len²）；首像素取精确值
        const float step = inverse_scale_;
        const float t_step = gdx * step / length_squared;
        const float first_x = (static_cast<float>(first_pixel) + 0.5f) * inverse_scale_;
        float position = ((first_x - from.x) * gdx + (logical_y - from.y) * gdy) / length_squared;
        for (int x = first_pixel; x < last_pixel; ++x, position += t_step) {
          const float covered = (x >= full_begin && x < full_end) ? weight : cover_of(x);
          if (covered <= kCoverageEpsilon) continue;
          blend_one(x, covered, gradient->sample_position(position));
        }
        continue;
      }
    }
    // 径向/扫掠（或异常几何）：逐像素采样
    const float step = inverse_scale_;
    float logical_x = (static_cast<float>(first_pixel) + 0.5f) * inverse_scale_;
    for (int x = first_pixel; x < last_pixel; ++x, logical_x += step) {
      const float covered = (x >= full_begin && x < full_end) ? weight : cover_of(x);
      if (covered <= kCoverageEpsilon) continue;
      blend_one(x, covered, paint.sample(math::Point{logical_x, logical_y}));
    }
  }
}

void Canvas::fill_path(const Path& path, const Paint& paint, DrawOptions options) {
  OpScope scope(*this, PaintOp::FillPath);
  if (scale_ == 1.0f) {
    detail::fill_path_aa(*this, path, paint, options);
    return;
  }
  detail::fill_path_aa(*this, path.scaled(scale_), paint, options);
}

void Canvas::fill_rect(math::Rect rect, const Paint& paint, float radius, DrawOptions options) {
  OpScope scope(*this, radius > 0.0f ? PaintOp::FillRoundRect : PaintOp::FillRect);
  const double rect_area = static_cast<double>(rect.width) * static_cast<double>(rect.height) *
                           static_cast<double>(scale_) * static_cast<double>(scale_);
  scope.set_pixels(rect_area > 0.0 ? static_cast<std::uint64_t>(rect_area) : 0);
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
  OpScope scope(*this, PaintOp::FillPath);
  if (radius <= 0.0f) return;
  if (scale_ != 1.0f) {
    center = to_physical(center);
    radius *= scale_;
  }
  Path path;
  path.add_circle(center, radius);
  detail::fill_path_aa(*this, path, paint, options);
}

void Canvas::stroke_path(const Path& path, const Paint& paint, float width,
                         const StrokeStyle& style, DrawOptions options) {
  OpScope scope(*this, PaintOp::Stroke);
  if (width <= 0.0f || path.is_empty()) return;
  // DPI：描边宽度同比例放大，1px 发丝线在 2x 屏上是 2 物理像素（视觉等宽且更锐利）
  const Path& source = path;
  const float physical_width_value = width * scale_;
  Path scaled_path = scale_ == 1.0f ? Path{} : source.scaled(scale_);
  const Path& effective = scale_ == 1.0f ? source : scaled_path;
  Path outline = detail::stroke_to_path(effective, physical_width_value,
                                        options.antialias ? 0.25f : 0.5f, style);
  detail::fill_path_aa(*this, outline, paint, options);
}

void Canvas::draw_shadow(math::Rect rect, float radius, float blur, math::Color color,
                         math::Point offset, DrawOptions options) {
  OpScope scope(*this, PaintOp::Shadow);
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

  // 形状与模糊**只取决于几何参数**：同尺寸的卡片每帧重算同一张遮罩是纯浪费，改走缓存。
  const std::shared_ptr<const Mask> mask = shadow_mask(rect.width, rect.height, radius, blur, offset);
  if (mask == nullptr || mask->empty()) return;
  scope.set_pixels(static_cast<std::uint64_t>(area.width) * static_cast<std::uint64_t>(area.height));
  const math::IntRect mask_origin = region.round_out();
  const std::span<const std::uint8_t> mask_values = mask->values();
  const int mask_width = mask->width();

  auto* base = pixels_.data();
  const std::uint32_t premul = math::premultiply(color);
  const bool masked = has_mask_clip();
  // **查表代替逐像素算术**：遮罩取值只有 256 种，而“遮罩 → α → 按 α 缩放的预乘源色”
  // 本身与像素位置无关。实测这段逐像素算术（浮点除法 + 4 次整数除法）占阴影开销的大半。
  //
  // 为什么不用 SIMD：这里试过一条“变 alpha 的向量化合成”（`pmullw` 逐通道算 v*a/255），
  // 实测**反而慢 1.76 倍**（同一会话交替测量：查表 7.5 ms / SIMD 13.1 ms，
  // 总帧 13.2 ms / 19.0 ms）。“只有 256 种取值”这个先验比向量宽度值钱得多——
  // 与其花 25 条 SSE 指令算 4 个像素的乘法，不如一次 L1 查表。
  // 保留此结论供后人参考：软件光栅器里“查表 vs 向量”要实测，别默认后者更快。
  std::array<std::uint32_t, 256> scaled_sources{};
  for (std::size_t level = 0; level < scaled_sources.size(); ++level) {
    const float normalized = static_cast<float>(level) / 255.0f * options.opacity;
    const auto alpha_byte =
        static_cast<std::uint32_t>(math::clamp01(normalized) * 255.0f + 0.5f);
    scaled_sources[level] = scale_premul(premul, alpha_byte);
  }
  for (int y = area.y; y < area.bottom(); ++y) {
    const int mask_y = y - mask_origin.y;
    if (mask_y < 0 || mask_y >= mask->height()) continue;
    const std::uint8_t* const mask_row =
        mask_values.data() + static_cast<std::size_t>(mask_y) * static_cast<std::size_t>(mask_width);
    auto* row = base + static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_);
    // 遮罩与目标 1:1 对齐（原实现走双线性采样：4 次读 + 6 次浮点，而这里是个恒等映射）
    const int x_begin = std::max(area.x, mask_origin.x);
    const int x_end = std::min(area.right(), mask_origin.x + mask_width);
    if (x_begin >= x_end) continue;

    if (!masked && options.blend == BlendMode::SrcOver) {
      for (int x = x_begin; x < x_end; ++x) {
        const std::uint8_t mask_byte = mask_row[x - mask_origin.x];
        if (mask_byte == 0U) continue;
        row[x] = over_premul(row[x], scaled_sources[mask_byte]);
      }
      continue;
    }
    for (int x = x_begin; x < x_end; ++x) {
      const std::uint8_t mask_byte = mask_row[x - mask_origin.x];
      if (mask_byte == 0U) continue;
      float alpha = static_cast<float>(mask_byte) / 255.0f * options.opacity;
      if (masked) {
        alpha = effective_alpha(x, y, alpha);
        if (alpha <= kCoverageEpsilon) continue;
      }
      row[x] = blend_pixel_premul(row[x], premul, alpha, options.blend);
    }
  }
}

auto Canvas::shadow_mask(float width, float height, float radius, float blur, math::Point offset)
    -> std::shared_ptr<const Mask> {
  // 键：几何参数按 0.25 像素量化（帧间的浮点噪声不该让缓存永远命不中）
  const auto quantize = [](float value) -> std::uint64_t {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(value * 4.0f)) +
                                      0x40000000);
  };
  std::uint64_t key = 1469598103934665603ULL;  // FNV-1a 64 偏移基
  for (const std::uint64_t part : {quantize(width), quantize(height), quantize(radius),
                                   quantize(blur), quantize(offset.x), quantize(offset.y)}) {
    key ^= part;
    key *= 1099511628211ULL;
  }
  for (const auto& [cached_key, cached] : shadow_masks_) {
    if (cached_key == key) return cached;
  }

  // 典型界面里阴影种类不多（卡片尺寸就那几种）；缓存满了丢最早一项，不引入 LRU 复杂度。
  constexpr std::size_t kShadowMaskCacheLimit = 48;
  if (shadow_masks_.size() >= kShadowMaskCacheLimit) shadow_masks_.erase(shadow_masks_.begin());

  // 遮罩在**规范化坐标**（矩形左上角为 0,0 + 偏移）下光栅化后模糊：
  // 因此它与绘制位置无关，可以直接跨元素复用。
  const float padding = blur * 2.0f + 2.0f;
  const math::Rect region =
      math::Rect{0.0f, 0.0f, width, height}.offset(offset.x, offset.y).inflate(padding);
  const math::IntRect region_int = region.round_out();
  if (region_int.is_empty()) return nullptr;
  auto mask = std::make_shared<Mask>(region_int.width, region_int.height);
  const math::Rect local{offset.x - static_cast<float>(region_int.x),
                         offset.y - static_cast<float>(region_int.y), width, height};
  detail::rasterize_mask(*mask, make_rounded_rect(local, radius), 0.0f, 0.0f);
  blur_mask(*mask, blur);
  shadow_masks_.emplace_back(key, mask);
  return mask;
}

auto Canvas::layered_shadow(float width, float height, float radius, math::Color key_color,
                            float key_blur, math::Point key_offset, math::Color ambient_color,
                            float ambient_blur, math::Point ambient_offset) -> const LayeredShadow* {
  // 键：几何 + 两层的模糊/偏移/颜色（同样的 0.25 像素量化；颜色按字节精确参与）。
  const auto quantize = [](float value) -> std::uint64_t {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(value * 4.0f)) +
                                      0x40000000);
  };
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](std::uint64_t part) {
    hash ^= part;
    hash *= 1099511628211ULL;
  };
  for (const std::uint64_t part :
       {quantize(width), quantize(height), quantize(radius), quantize(key_blur),
        quantize(key_offset.x), quantize(key_offset.y), quantize(ambient_blur),
        quantize(ambient_offset.x), quantize(ambient_offset.y)}) {
    mix(part);
  }
  mix(static_cast<std::uint64_t>(key_color.r) | (static_cast<std::uint64_t>(key_color.g) << 8U) |
      (static_cast<std::uint64_t>(key_color.b) << 16U) |
      (static_cast<std::uint64_t>(key_color.a) << 24U));
  mix(static_cast<std::uint64_t>(ambient_color.r) |
      (static_cast<std::uint64_t>(ambient_color.g) << 8U) |
      (static_cast<std::uint64_t>(ambient_color.b) << 16U) |
      (static_cast<std::uint64_t>(ambient_color.a) << 24U));
  for (const auto& [cached_key, cached] : layered_shadows_) {
    if (cached_key == hash) return &cached;
  }

  constexpr std::size_t kLayeredShadowLimit = 48;
  if (layered_shadows_.size() >= kLayeredShadowLimit) {
    layered_shadows_.erase(layered_shadows_.begin());
  }

  // 每一层各在自己的局部坐标里光栅化（与 `shadow_mask` 同一口径），再取两层的**并集**区域：
  // 两层的非零区高度重叠，取并集才让"重叠处只合成一次"这个收益真正落地。
  struct Local {
    std::shared_ptr<const Mask> mask{};
    int x{0};
    int y{0};
    int width{0};
    int height{0};
    math::Color color{};
    float opacity{1.0f};
  };
  const auto make_local = [&](float blur, math::Point offset, math::Color color) -> Local {
    const float padding = blur * 2.0f + 2.0f;
    const math::IntRect region =
        math::Rect{0.0f, 0.0f, width, height}.offset(offset.x, offset.y).inflate(padding)
            .round_out();
    if (region.is_empty()) return Local{};
    auto mask = std::make_shared<Mask>(region.width, region.height);
    const math::Rect local{offset.x - static_cast<float>(region.x),
                           offset.y - static_cast<float>(region.y), width, height};
    detail::rasterize_mask(*mask, make_rounded_rect(local, radius), 0.0f, 0.0f);
    blur_mask(*mask, blur);
    return Local{mask, region.x, region.y, region.width, region.height, color, 1.0f};
  };

  const Local ambient = make_local(ambient_blur, ambient_offset, ambient_color);
  const Local key = make_local(key_blur, key_offset, key_color);
  if (key.mask == nullptr && ambient.mask == nullptr) return nullptr;

  LayeredShadow entry;
  entry.x = std::min(key.mask != nullptr ? key.x : ambient.x,
                     ambient.mask != nullptr ? ambient.x : key.x);
  entry.y = std::min(key.mask != nullptr ? key.y : ambient.y,
                     ambient.mask != nullptr ? ambient.y : key.y);
  const int right = std::max(key.mask != nullptr ? key.x + key.width : ambient.x + ambient.width,
                             ambient.mask != nullptr ? ambient.x + ambient.width
                                                     : key.x + key.width);
  const int bottom =
      std::max(key.mask != nullptr ? key.y + key.height : ambient.y + ambient.height,
               ambient.mask != nullptr ? ambient.y + ambient.height : key.y + key.height);
  entry.width = right - entry.x;
  entry.height = bottom - entry.y;
  if (entry.width <= 0 || entry.height <= 0) return nullptr;

  auto pixels = std::make_shared<std::vector<std::uint32_t>>(
      static_cast<std::size_t>(entry.width) * static_cast<std::size_t>(entry.height), 0U);
  // ⚠ 从**全透明**开始合成，贴图存的是"阴影自身的预乘色"——不能烘进任何目标底色
  // （烘了的话，元素底下是渐变/别的元素时贴上去就是错的）。
  // src-over 满足结合律 ⇒ "先合并两层再贴" 与 "依次贴两次" 逐像素等价。
  const auto paint = [&](const Local& layer) {
    if (layer.mask == nullptr) return;
    const std::uint32_t premul = math::premultiply(layer.color);
    std::array<std::uint32_t, 256> scaled{};
    for (std::size_t level = 0; level < scaled.size(); ++level) {
      const float normalized = static_cast<float>(level) / 255.0f * layer.opacity;
      scaled[level] = scale_premul(
          premul, static_cast<std::uint32_t>(math::clamp01(normalized) * 255.0f + 0.5f));
    }
    const auto values = layer.mask->values();
    const int mask_width = layer.mask->width();
    const int mask_height = layer.mask->height();
    const int y_begin = std::max(entry.y, layer.y);
    const int y_end = std::min(entry.y + entry.height, layer.y + layer.height);
    const int x_begin = std::max(entry.x, layer.x);
    const int x_end = std::min(entry.x + entry.width, layer.x + layer.width);
    for (int y = y_begin; y < y_end; ++y) {
      const int local_y = y - layer.y;
      if (local_y < 0 || local_y >= mask_height) continue;
      const std::uint8_t* mask_row =
          values.data() + static_cast<std::size_t>(local_y) * static_cast<std::size_t>(mask_width);
      auto* row = pixels->data() + static_cast<std::size_t>(y - entry.y) *
                                       static_cast<std::size_t>(entry.width);
      for (int x = x_begin; x < x_end; ++x) {
        const std::uint8_t mask_byte = mask_row[x - layer.x];
        if (mask_byte == 0U) continue;
        row[x - entry.x] = over_premul(row[x - entry.x], scaled[mask_byte]);
      }
    }
  };
  // 先环境层（大而淡）、后关键层（紧而实）——与 `Element::paint_box` 的视觉定义一致。
  paint(ambient);
  paint(key);

  auto spans = std::make_shared<std::vector<std::pair<int, int>>>(
      static_cast<std::size_t>(entry.height), std::pair<int, int>{-1, -1});
  for (int row = 0; row < entry.height; ++row) {
    const auto* source = pixels->data() + static_cast<std::size_t>(row) *
                                              static_cast<std::size_t>(entry.width);
    for (int column = 0; column < entry.width; ++column) {
      if (source[column] == 0U) continue;
      if ((*spans)[static_cast<std::size_t>(row)].first < 0) {
        (*spans)[static_cast<std::size_t>(row)].first = column;
      }
      (*spans)[static_cast<std::size_t>(row)].second = column;
    }
  }
  entry.pixels = std::move(pixels);
  entry.spans = std::move(spans);
  layered_shadows_.emplace_back(hash, std::move(entry));
  return &layered_shadows_.back().second;
}

void Canvas::draw_shadow_layered(math::Rect rect, float radius, math::Color key_color,
                                 float key_blur, math::Point key_offset, math::Color ambient_color,
                                 float ambient_blur, math::Point ambient_offset,
                                 DrawOptions options) {
  OpScope scope(*this, PaintOp::Shadow);
  if (rect.is_empty()) return;
  // DPI：几何与偏移按 scale 放大（与 `draw_shadow` 同一口径）。
  if (scale_ != 1.0f) {
    rect = math::Rect{rect.x * scale_, rect.y * scale_, rect.width * scale_, rect.height * scale_};
    radius *= scale_;
    key_blur *= scale_;
    ambient_blur *= scale_;
    key_offset = math::Point{key_offset.x * scale_, key_offset.y * scale_};
    ambient_offset = math::Point{ambient_offset.x * scale_, ambient_offset.y * scale_};
  }
  if (clip_rect().is_empty()) return;

  const bool has_key = key_color.a != 0U && key_blur > 0.0f;
  const bool has_ambient = ambient_color.a != 0U && ambient_blur > 0.0f;
  if (!has_key && !has_ambient) return;
  if (!has_ambient) {
    draw_shadow(rect, radius, key_blur, key_color, key_offset, options);
    return;
  }
  if (!has_key) {
    draw_shadow(rect, radius, ambient_blur, ambient_color, ambient_offset, options);
    return;
  }

  // 贴图在**规范化坐标**下缓存（与绘制位置无关）——键里已含几何与两层颜色。
  const LayeredShadow* sprite = layered_shadow(rect.width, rect.height, radius, key_color,
                                               key_blur, key_offset, ambient_color, ambient_blur,
                                               ambient_offset);
  if (sprite == nullptr || sprite->pixels == nullptr) return;

  // 贴图原点 = 规范化区域 + 本次绘制位置（round_out 与缓存里的取整口径一致）。
  const int origin_x = static_cast<int>(std::lround(rect.x)) + sprite->x;
  const int origin_y = static_cast<int>(std::lround(rect.y)) + sprite->y;
  const math::IntRect clip = clip_rect();
  const bool masked = has_mask_clip();
  const float opacity = options.opacity;
  const bool plain = !masked && options.blend == BlendMode::SrcOver && opacity >= 0.999f;
  scope.set_pixels(static_cast<std::uint64_t>(sprite->width) *
                   static_cast<std::uint64_t>(sprite->height));

  const auto* const pixels = sprite->pixels.get();
  const auto* const spans = sprite->spans.get();
  for (int row = 0; row < sprite->height; ++row) {
    const int y = origin_y + row;
    if (y < clip.y || y >= clip.bottom()) continue;
    const auto [first, last] = (*spans)[static_cast<std::size_t>(row)];
    if (first < 0) continue;
    const auto* src =
        pixels->data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(sprite->width);
    auto* dst = pixels_.data() + static_cast<std::size_t>(y) *
                                     static_cast<std::size_t>(physical_width_);
    // 行内非零跨度与裁剪区求交（一次算好，避免逐像素判边界）。
    const int x_begin = std::max(origin_x + first, clip.x);
    const int x_end = std::min(origin_x + last + 1, clip.right());
    if (x_begin >= x_end) continue;
    if (plain) {
      for (int x = x_begin; x < x_end; ++x) {
        const std::uint32_t source = src[x - origin_x];
        if (source == 0U) continue;
        dst[x] = over_premul(dst[x], source);
      }
      continue;
    }
    for (int x = x_begin; x < x_end; ++x) {
      const std::uint32_t source = src[x - origin_x];
      const std::uint32_t source_alpha = channel(source, 0);
      if (source_alpha == 0U) continue;
      float alpha = static_cast<float>(source_alpha) / 255.0f * opacity;
      if (masked) {
        alpha = effective_alpha(x, y, alpha);
        if (alpha <= kCoverageEpsilon) continue;
      }
      // 非常规合成路径：把贴图像素还原成预乘源 × α 的形式再走通用混合。
      const auto scale_alpha = [alpha](std::uint32_t value) noexcept -> std::uint32_t {
        return fast_div255(value * static_cast<std::uint32_t>(math::clamp01(alpha) * 255.0f +
                                                              0.5f) +
                           127U);
      };
      const std::uint32_t scaled =
          pack(scale_alpha(channel(source, 24)), scale_alpha(channel(source, 16)),
               scale_alpha(channel(source, 8)), scale_alpha(source_alpha));
      dst[x] = blend_pixel_premul(dst[x], scaled, alpha, options.blend);
    }
  }
}

void Canvas::draw_canvas(const Surface& source, math::Rect destination, DrawOptions options) {
  OpScope scope(*this, PaintOp::Image);
  const int source_width = source.physical_width();
  const int source_height = source.physical_height();
  if (destination.is_empty() || source_width <= 0 || source_height <= 0) return;
  // destination 为逻辑坐标 → 物理像素；采样源为其物理缓冲
  const math::IntRect area = to_physical(destination).intersect(clip_rect());
  if (area.is_empty()) return;
  const math::Rect physical_destination =
      scale_ == 1.0f
          ? destination
          : math::Rect{destination.x * scale_, destination.y * scale_, destination.width * scale_,
                       destination.height * scale_};
  const float scale_x = static_cast<float>(source_width) / physical_destination.width;
  const float scale_y = static_cast<float>(source_height) / physical_destination.height;
  // 源像素经接口取（**不假设对方也是软件画布**：GPU 目标会在这里触发回读，
  // 慢但正确——`draw_canvas` 是截图/离屏合成的路径，不在每帧热路径上）
  const std::span<const std::uint32_t> source_pixels = source.pixels();

  for (int y = area.y; y < area.bottom(); ++y) {
    auto* row = pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_);
    const float source_y =
        (static_cast<float>(y) + 0.5f - physical_destination.y) * scale_y - 0.5f;
    for (int x = area.x; x < area.right(); ++x) {
      const float source_x =
          (static_cast<float>(x) + 0.5f - physical_destination.x) * scale_x - 0.5f;
      const float clamped_x =
          math::clampf(source_x, 0.0f, static_cast<float>(source_width - 1));
      const float clamped_y =
          math::clampf(source_y, 0.0f, static_cast<float>(source_height - 1));
      const auto x0 = static_cast<int>(clamped_x);
      const auto y0 = static_cast<int>(clamped_y);
      const int x1 = x0 + 1 < source.physical_width() ? x0 + 1 : x0;
      const int y1 = y0 + 1 < source.physical_height() ? y0 + 1 : y0;
      const float fx = clamped_x - static_cast<float>(x0);
      const float fy = clamped_y - static_cast<float>(y0);
      const auto sample = [&source_pixels, source_width](int sx, int sy) noexcept -> std::uint32_t {
        return source_pixels[static_cast<std::size_t>(sy) * static_cast<std::size_t>(source_width) +
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

void Canvas::draw_canvas_at(const Surface& source, int x, int y, DrawOptions options) {
  OpScope scope(*this, PaintOp::Image);
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
  OpScope scope(*this, PaintOp::ClipMask);
  // 入参是**逻辑坐标**：换算到物理后再构造路径（否则 2x 屏上裁剪区域只覆盖左上 1/4，
  // 子项会被整片裁掉——表现为"卡片里的内容凭空消失"）。
  const math::Rect physical = math::Rect{rect.x * scale_, rect.y * scale_, rect.width * scale_,
                                         rect.height * scale_};
  Path path;
  path.add_rounded_rect(physical, radius * scale_);
  push_clip_path(path);
}

void Canvas::push_clip_path(const Path& path) {
  OpScope scope(*this, PaintOp::ClipMask);
  // 与 `push_clip_rect` 一致：**路径按物理像素解释**（raster 层其余 API 处理的是物理像素；
  // 逻辑坐标的换算在 UI 侧进入画布接口时完成）。调用方若持有逻辑坐标路径，请先 `scaled(dpr)`。
  const math::Rect bounds = path.flattened_bounds(0.25f);
  // `inflate` 的入参是**整数**像素：写 `1.0f` 会触发 float→int 隐式转换
  // （MSVC `/W4` 下是 C4244，本仓库把它当错误）——整条路径本来就只应在整数像素上扩张。
  const math::IntRect area = bounds.round_out().inflate(1).intersect(clip_rect());
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
