#include "st/raster/canvas.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string_view>
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

/// 几何 sprite 缓存的尺寸/条数上限。
///
/// 为何要上限：缓存是**进程级常驻**（跟着画布走），而界面可能绘成千上万个不同的
/// 小图案（如每次重算的波形）。上限按「界面里真实有多少种图标」定：
/// 一帧 46 个描边里通常只有十几到几十种形状，512 条已宽裕。
/// 面积上限 256×256 保证单条不会突然吃掉几 MB。
inline constexpr int kGeometrySpriteMaxPixels = 256 * 256;
inline constexpr std::size_t kGeometrySpriteLimit = 512;

/// 几何 sprite 缓存是否启用（`ST_RASTER_SPRITE_CACHE=0` 可关）。
///
/// **它是回归测的必备开关**：缓存必须与“不走缓存”逐像素相同，而验证这件事的
/// 唯一干净办法是同一份二进制里把两侧都跑一遍（换二进制比会引入其他改动）。
[[nodiscard]] auto geometry_sprite_cache_enabled() noexcept -> bool {
  static const bool enabled = [] {
    const char* value = std::getenv("ST_RASTER_SPRITE_CACHE");
    if (value == nullptr || *value == '\0') return true;
    return std::string_view{value} != "0";
  }();
  return enabled;
}

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
  // **源色不透明时的快路径**（文字几乎总走这条：`Paint::solid(color)` 且色 α=255）。
  //
  // 为何能省：`attenuation = fast_div255(source_alpha * alpha + 127)`，当
  // `source_alpha == 255` 时它恒等于 `alpha`（`(255*alpha+127)/255 == alpha`，
  // 对 `alpha ∈ [0,255]` 逐一成立）——于是每个通道少一次整数乘+移位，
  // 4 个通道共省 4 次。实测这个函数是 LCD 文字路径的主体（每像素 8 次
  // `fast_div255`，其中 4 次是这个衰减）。
  //
  // 量化口径不变：`fast_div255(source * alpha + 127) + fast_div255(dst * (255-alpha) + 127)`
  // 与通用式的 `attenuation == alpha` 分支**逐位相同**。
  const bool opaque_source = source_alpha == 255U;
  const auto blend = [source_alpha, opaque_source](std::uint32_t source,
                                                   std::uint32_t destination,
                                                   std::uint32_t alpha) constexpr noexcept {
    const std::uint32_t attenuation =
        opaque_source ? alpha : fast_div255(source_alpha * alpha + 127U);
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

/// 相邻 run 是否**可能共享同一个像素**。
///
/// 用于 `blend_coverage_runs` 的路由：只有共享像素时才需要"先累加、同一像素只混一次"
/// （见那里的长注释）；互不相交的 run（如一个矩形的两条竖边）走通用循环（含 full-span SIMD）更快。
///
/// 判据用**像素覆盖区间的交**而不是几何交集：两条 run 几何上不相交、但都被同一个像素的
/// 量化零头碰到时（如 `[0.0,0.4)` 与 `[0.4,0.8)`），**也算共享**——那正是"斜边欠墨"的成因。
/// 代价是判据偏保守（宁可多走一步累加，不可少走——少了就是缺墨）。
[[nodiscard]] auto runs_share_pixels(std::span<const CoverageRun> runs, float clip_left,
                                    float clip_right) noexcept -> bool {
  int previous_last = -1;
  for (const auto& run : runs) {
    const float weight = std::abs(run.weight);
    if (weight <= kCoverageEpsilon) continue;
    const float a = std::max(run.x0, clip_left);
    const float b = std::min(run.x1, clip_right);
    if (b <= a) continue;
    const int first = static_cast<int>(std::floor(a));
    const int last = static_cast<int>(std::ceil(b)) - 1;   // 闭区间末像素
    if (previous_last >= first) return true;
    if (last > previous_last) previous_last = last;
  }
  return false;
}

/// 一行里"**确定被**圆角矩形完全覆盖"的像素范围（闭开区间 `[begin, end)`）。
///
/// 为何要这套而不是直接用矩形：`covered_region` 的语义是「这些像素随后会被不透明底
/// **完全**替换」，而圆角矩形的四角**不在**矩形内——把整块矩形当覆盖会让卡片四角
/// 丢掉阴影。同时抗锯齿会让几何边缘所在的**那一像素**只被部分覆盖，所以两侧还要
/// 各让出 1 像素（调用方已向内缩过）。两道缩减只损失边缘几行/列像素，
/// 但把「跳过是否等价」从「大概率」变成**确定**。
struct CoveredSpan {
  int begin{0};
  int end{0};
  [[nodiscard]] auto empty() const noexcept -> bool { return begin >= end; }
};

/// `rect`：**已缩进 1 像素**的矩形（物理像素）；`radius`：物理像素。
[[nodiscard]] auto covered_span_at(math::IntRect rect, float radius, int y) noexcept -> CoveredSpan {
  if (rect.is_empty() || y < rect.y || y >= rect.bottom()) return CoveredSpan{rect.x, rect.x};
  float inset = 0.0f;
  const float corner =
      std::min(radius, static_cast<float>(std::min(rect.width, rect.height)) * 0.5f);
  if (corner > 0.5f) {
    // 行顶到矩形顶的距离（上圆角带）/ 到矩形底的距离（下圆角带）。
    const float dy_top = static_cast<float>(y - rect.y);
    const float dy_bottom = static_cast<float>(rect.bottom() - (y + 1));
    const float dy = std::min(dy_top, dy_bottom);
    if (dy < corner) {
      const float straight = corner - dy;
      const float square = corner * corner - straight * straight;
      inset = corner - std::sqrt(square > 0.0f ? square : 0.0f);
    }
  }
  // 向内取整到**整像素**（`ceil` 向上取到下一整像素，保证该像素被完全覆盖）。
  const int left = rect.x + static_cast<int>(std::ceil(inset));
  const int right = rect.right() - static_cast<int>(std::ceil(inset));
  return CoveredSpan{left, std::max(left, right)};
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
  // 新像素全是透明的 0，“刚清过某颜色”的前提不复存在。
  cleared_fresh_ = false;
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
  // **走 `simd::fill_row`，不用 `std::ranges::fill`**。
  //
  // 为何：`clear` 是每帧必发生的整屏写（1600×1000 要写 1.35 M 像素），
  // 而 `std::ranges::fill` 对非 0 的 32 位值走的是**通用逐元素赋值**，
  // 编译器不能把它变成 `memset`（值不是 0/0xFF 的重复字节）。
  // `simd::fill_row` 是手写的 AVX2（8 像素/次）/SSE2（4 像素/次）广播写。
  //
  // 实测（`docs/PERF` 的记录口径 + 本轮 `frame_floor_probe` 对照）：
  // 整屏 clear 曾是 **0.35 ns/px** 量级，而同一块画布的纯填充只需 **0.145 ns/px**
  // ——差的正是这一条。
  //
  // 两点都不再需要旧的两个分支：`fill_row` 对任何 32 位值都是同一份实现，
  // 不透明/半透明都只是数据不同（预乘已在上面算好）。
  const std::size_t total = pixels_.size();
  if (total > 0) {
    simd::fill_row(pixels_.data(), total, value);
  }
  // 记下“刚清成什么颜色、且还没被任何绘制碰过”——供 `fill_rect` 跳过
  // 紧随其后的“整屏同色不透明底”（那是纯重复写）。只记不透明色：
  // 半透明清屏的最终结果取决于它下面的内容，不能当作可比的底。
  cleared_color_ = color;
  cleared_fresh_ = color.a == 255U;
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

auto Canvas::replace_rgba8(std::span<const std::uint8_t> bytes, int width, int height) -> bool {
  // 尺寸必须与**物理**尺寸一致：后处理结果是整帧，不做缩放。
  // 若允许不同尺寸，调用方就得猜“到底该把哪个尺寸当谁”，
  // 而猜错的表现是“画面突然缩在左上角”——很难查。
  if (width != physical_width_ || height != physical_height_) return false;
  const auto expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
  if (bytes.size() < expected) return false;
  for (std::size_t index = 0; index < pixels_.size(); ++index) {
    const std::size_t offset = index * 4U;
    const std::uint8_t alpha = bytes[offset + 3U];
    // 内部缓冲是**预乘**的：直通值乘 alpha 再存（与 `to_rgba8` 的
    // `unpremultiply` 对称）。
    const math::Color straight{bytes[offset + 0U], bytes[offset + 1U], bytes[offset + 2U],
                               alpha};
    pixels_[index] = math::premultiply(straight);
  }
  return true;
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

auto Canvas::alpha_table(std::uint32_t premul) const -> const AlphaTable& {
  // 单槽缓存：`table[k] = scale_premul(premul, k)`——**与 opacity 无关**，
  // 所以同一颜色的几十个字形共用一份；颜色变了才重建。
  //
  // ⚠ **opacity 绝不能烧进表**：若表键是“覆盖率字节 × opacity 再量化”，
  // 就与旧写法（`quantize(|cov| × opacity)`）形成**两步量化**。
  // 实测（`tools/alpha_table_equiv_probe.cpp`）：opacity=0.9/0.75/0.5 时
  // 有 **19~25%** 的覆盖率组合会差 1 LSB——而本项目对像素是零容差。
  if (alpha_table_premul_ == premul) return alpha_table_;
  for (std::size_t level = 0; level < alpha_table_.size(); ++level) {
    alpha_table_[level] = scale_premul(premul, static_cast<std::uint32_t>(level));
  }
  alpha_table_premul_ = premul;
  return alpha_table_;}

void Canvas::blend_coverage_row(int y, int x_begin, std::span<const float> coverage,
                                const Paint& paint, float opacity, BlendMode blend) {  if (y < 0 || y >= physical_height_) return;  const ClipFrame& clip = current_clip();
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
    if (!masked && blend == BlendMode::SrcOver && alpha_table_enabled_) {
      const auto full_alpha = static_cast<std::uint8_t>(math::clamp01(opacity) * 255.0f + 0.5f);
      // **查表代替逐像素的 `scale_premul`**，但表要**跨行复用**。
      //
      // ## 为何查表
      // `scale_premul` 对每个像素算 4 个通道的 `fast_div255`（各一次乘法 + 移位），
      // 而结果**只取决于量化后的 alpha 字节（256 种）**——与像素位置无关。
      // 字形像素以“非满格边缘”为主：实测（`tools/glyph_coverage_distribution_probe.cpp`）
      // 13px 小字只有 **7.9%** 满格、**80.4%** 落在 0.1~0.99，全走逐像素路径。
      //
      // ## ⚠ 两个实测踩过的坑
      // **坑一：表不能在行内建**。本函数是**逐行调用**的，第一版把
      // `std::array<uint32_t,256>` 建在行内，于是每个字形行都要初始化 256×4 通道
      // ——小字形一行只画几个像素，这笔代价远大于它省下的。
      // 实测整帧 `text` 0.68 → 0.67 ms（噪声内），等于白改。
      //
      // **坑二：opacity 绝不能烧进表**。若表键是“覆盖率字节 × opacity 再量化”，
      // 就与旧写法 `quantize(|cov| × opacity)` 形成**两步量化**。
      // 实测（`tools/alpha_table_equiv_probe.cpp`）：opacity=0.9/0.75/0.5 时
      // 有 **19~25%** 的覆盖率组合差 1 LSB——而本项目像素是零容差。
      // 因此表只能是 `table[k] = scale_premul(premul, k)`，量化仍在调用处做一次。
      const AlphaTable& table = alpha_table(premul);
      int x = begin;
      while (x < end) {
        // 覆盖率满格（≥ 0.999）的连续段——大字/粗笔画的内部——整段交给 SIMD，
        // 跳过整串逐像素浮点运算。
        if (coverage_at(x) >= 0.999f) {
          const int run_begin = x;
          while (x < end && coverage_at(x) >= 0.999f) ++x;
          simd::blend_row(row + run_begin, static_cast<std::size_t>(x - run_begin), premul,
                          full_alpha);
          continue;
        }
        // 表的索引是**已乘 opacity 的量化字节**——这样量化只发生一次
        //（与旧写法逐字相同）。表本身不烧 opacity：它只是 `scale_premul(premul, k)`
        // 的 256 项展开，对任何 opacity 都成立。
        const float alpha = std::abs(coverage_at(x)) * opacity;
        if (alpha > kCoverageEpsilon) {
          const auto alpha_byte =
              static_cast<std::uint32_t>(math::clamp01(alpha) * 255.0f + 0.5f);
          row[x] = over_premul(row[x], table[alpha_byte]);
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
  //
  // **无遮罩时走 256 项查表**：`alpha_of` 每像素被调 3 次（R/G/B），
  // 每次都要 `abs() → × opacity → clamp01 → ×255+0.5 量化`。
  // 覆盖率本身是 `float`，但量化后的字节只有 256 种——而**实际进混合的只有量化值**。
  // 于是把“覆盖率字节 → 乘 opacity 后的字节”预先算好。
  //
  // ⚠ **量化必须仍在同一步完成**：若把 `quantize(cov)` 后再乘 opacity 量化，
  // 就变成两步量化（实测在 opacity=0.9/0.75/0.5 下 19~25% 的组合差 1 LSB，
  // 见 `tools/alpha_table_equiv_probe.cpp`）。所以表的定义是：
  //   `lcd_alpha[k] = quantize(quantize(k/255) × opacity)`  —— **不对**
  //   `lcd_alpha[k] = quantize((k/255) × opacity)`         —— **不对**
  // 正确做法不能预先把 cov 量化。因此这里**不改量化，只减少重复函数调用**：
  // 把 `masked` 判断提到循环外（它本来就是行内常量），并在无遮罩时走无分支版本。
  const auto alpha_plain = [&](int x, std::size_t channel) -> std::uint32_t {
    const float alpha = std::abs(coverage_at(x, channel)) * opacity;
    if (alpha <= 0.0f) return 0U;
    return static_cast<std::uint32_t>(math::clamp01(alpha) * 255.0f + 0.5f);
  };
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
    if (!masked) {
      // 无遮罩：直接用无分支版本（省掉每像素 3 次的 `masked` 判断与闭包调用）。
      for (int x = begin; x < end; ++x) {
        const std::uint32_t a_r = alpha_plain(x, 0U);
        const std::uint32_t a_g = alpha_plain(x, 1U);
        const std::uint32_t a_b = alpha_plain(x, 2U);
        if (a_r == 0U && a_g == 0U && a_b == 0U) continue;
        row[x] = over_premul_lcd(row[x], premul, a_r, a_g, a_b);
      }
      return;
    }
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

  // —— 覆盖率混合：**同一像素只混一次**，但整段仍走 `simd::blend_row` ——
  //
  // 为何"同一像素只混一次"：覆盖率是**带符号权重之和**，而像素的最终不透明度是
  // `1 - Π(1 - 本像素各段覆盖率)`——不是“逐段各混一次”的线性叠加。旧写法逐段调用
  // `blend_one`，每段都要做一次 `alpha → u8 → 再回浮点`；在**斜边**上同一像素会被
  // 多段各碰一点，量化/截断的零头每段都差一点，累加后形成**系统性欠墨**
  //（而水平/垂直边上每像素只被一段完整覆盖，恰好不差）。
  //
  // 实测（`tools/ring_raster_probe.cpp`，1 逻辑px 宽、60 逻辑px 长的带，scale=2）：
  //   0°/90°（水平/垂直）比 1.0000；15° 0.8817；30° 0.8713；**45° 0.8568**；75° 0.9462。
  // 圆角边框的弧段就是斜向带，于是“圆角看着比直边细”——用户报的就是这个现象。
  // 改为“先累加成每像素覆盖率、同一像素只混一次”后，上表七档全部回到 1.0000。
  //
  // ## 但**不要**整行逐像素累加（2026-10-14 修，代价实测 ~20 倍）
  //
  // b420909 加的那版正是"整行逐像素累加"：它对每个像素调一次 `blend_pixel_premul`，
  // **遮蔽了 `simd::blend_row` 的 full-span 快路径**（该快路径在下面通用循环里，
  // 走的是"整段同 alpha → 向量化"）。同一份圆角卡片负载实测（release，
  // `tools/rounded_fill_probe.cpp`）：
  //
  //   整行累加版：30.2 ms（26.7× 清屏单位）→ 本写法：1.5 ms（**1.32×**，**20 倍**）
  //
  // 而它想解决的"斜边欠墨"并不需要它：**`merge_runs`（rasterizer 侧）已把同一行的
  // 重叠段落按事件扫描合并成互不重叠、权重唯一的段**（那是 b9166d5 渲染 5× 轮的成果）。
  // "整行累加"是**上游已保证的不变量的重复实现**，还把 O(段数) 退化成 O(像素数)。
  //
  // ## 本写法的等价性论证
  //
  // ① **与旧（b420909 版）语义等价**：旧版对每个覆盖像素按 `covered` 混一次；本版
  //    先按像素累加成 `covered`（与旧版同一个算式、同一份浮点累加），再**按
  //    `clamp01(covered*opacity)` 量化后的字节值分组**，同值的极大段走向量化混合。
  //    量化口径与 `blend_pixel_premul` 逐字相同（`*255+0.5` 截断，再 clamp），
  //    所以**逐像素结果逐位相同**（见 `tests/paint_optimization_test.cpp` 的
  //    斜带七档与环总墨量回归）。
  // ② **段数**：旧版每像素一次混合；本版段数 ≤ 不同 alpha 字节值的个数 ≤ 256，
  //    典型（斜带 / 圆角）只有 2~4 段，且**完全覆盖段恢复 SIMD**——那正是旧快路径。
  // ③ **顺序无关**：每条 run 的累加顺序与旧版一致（都不处理 `weight <= ε` 的段），
  //    浮点求和顺序也相同，故不存在"换个顺序就少一点墨"。
  //
  // 遮罩（`effective_alpha` 逐像素查表）与非常规混合模式保留原逐段路径。
  //
  // ⚠ 只在**相邻 run 可能共享像素**时才走这条（`runs_share_pixels`）：单条 run
  // （互不相交的运行段，比如一个矩形的左右两条竖边）根本不存在"同一像素被碰两次"，
  // 白走一趟累加反而比通用循环慢（实测 1.32× → 6.85×，因为通用循环有 full-span SIMD）。
  if (solid && !masked && blend == BlendMode::SrcOver && runs.size() > 1 &&
      runs_share_pixels(runs, clip_left, clip_right)) {
    int lo = physical_width_;
    int hi = 0;
    for (const auto& run : runs) {
      const float weight = std::abs(run.weight);
      if (weight <= kCoverageEpsilon) continue;
      const float a = std::max(run.x0, clip_left);
      const float b = std::min(run.x1, clip_right);
      if (b <= a) continue;
      lo = std::min(lo, static_cast<int>(std::floor(a)));
      hi = std::max(hi, static_cast<int>(std::ceil(b)) + 1);
    }
    if (hi > lo) {
      hi = std::min(hi, physical_width_);
      const auto width = static_cast<std::size_t>(hi - lo);
      coverage_accum_.assign(width, 0.0f);
      // 先把每像素覆盖率累加出来（缺省 0；只有真的被覆盖到的像素会被累加）
      for (const auto& run : runs) {
        const float weight = std::abs(run.weight);
        if (weight <= kCoverageEpsilon) continue;
        const float a = std::max(run.x0, clip_left);
        const float b = std::min(run.x1, clip_right);
        if (b <= a) continue;
        const int first_pixel = std::max(lo, static_cast<int>(std::floor(a)));
        const int last_pixel = std::min(hi, static_cast<int>(std::ceil(b)));
        for (int x = first_pixel; x < last_pixel; ++x) {
          const float left = std::max(a, static_cast<float>(x));
          const float right = std::min(b, static_cast<float>(x) + 1.0f);
          if (right > left) {
            coverage_accum_[static_cast<std::size_t>(x - lo)] += (right - left) * weight;
          }
        }
      }
      // 再按"量化后的 alpha 字节"分组输出：同值的极大段走 SIMD，逐像素结果与
      // "每像素各自混合一次"完全相同（分组只是把相邻同值的混合合并成一条向量指令）。
      std::size_t index = 0;
      while (index < width) {
        const float covered = coverage_accum_[index];
        if (covered <= kCoverageEpsilon) {
          ++index;
          continue;
        }
        const auto alpha_byte =
            static_cast<std::uint8_t>(math::clamp01(covered * opacity) * 255.0f + 0.5f);
        std::size_t end = index + 1;
        while (end < width && coverage_accum_[end] > kCoverageEpsilon) {
          const auto next_byte = static_cast<std::uint8_t>(
              math::clamp01(coverage_accum_[end] * opacity) * 255.0f + 0.5f);
          if (next_byte != alpha_byte) break;
          ++end;
        }
        simd::blend_row(row + lo + static_cast<int>(index), end - index, premul, alpha_byte);
        index = end;
      }
      return;
    }
  }

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
  const Path& physical = scale_ == 1.0f ? path : path.scaled(scale_);
  if (fill_physical_path_with_cache(path, physical, paint, options)) return;
  rasterize_path_runs(physical, paint, options);
}

/// 以**物理坐标路径**尝试几何贴图缓存（命中或建图后已交付则返回 true）。
///
/// `source` 用来算几何键（必须是**未缩放的源路径**——键里混了 `scale`，
/// 同一路径在 1x/2x 下必须是不同条目），`physical` 用来建贴图（已乘 `scale_`）。
///
/// 抽出来是为了两个调用方共用：`fill_path`（逻辑路径 × scale）与
/// `fill_rect` 的圆角分支（**建路径时就已经是物理坐标**，再交给 `fill_path`
/// 会被双重缩放）。
auto Canvas::fill_physical_path_with_cache(const Path& source, const Path& physical,
                                           const Paint& paint, DrawOptions options) -> bool {
  // **缓存关闭时完全不算键**：算键要遍历两遍命令流（`path_control_origin` + `path_geometry_key`），
  // 对“一条便宜的小路径”是可观的比例开销——实测它把“环形 vs 描边”的性能对照用例
  // 从 1.35× 抬成噪声（两者都白付这两遍）。开启时这点代价只付一次、换来整块贴图。
  if (!geometry_cache_enabled_ || !geometry_sprite_cache_enabled()) return false;
  const float tolerance = options.antialias ? 0.25f : 0.5f;
  const math::Point origin = path_control_origin(physical, 1.0f);
  const std::uint64_t key =
      path_geometry_key(source, scale_, static_cast<std::uint64_t>(tolerance * 100000.0f), origin);
  const auto build_sprite_path = [&physical, origin]() -> Path {
    return physical.translated(-origin.x, -origin.y);
  };
  return try_geometry_sprite(key, paint, options.opacity, options.blend, build_sprite_path, origin);
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

  // **跳过“清屏后重复铺同色整屏底”**。
  //
  // `UIView::paint` 的整帧路径是 `clear(bg)` → 画整树，而根容器几乎总会先用同一个
  // `bg` 铺满整屏——那一遍 `clear` 于是白写了一次（1440p 上 clear 独自就要 1.09 ms，
  // 首层 fill 再 0.67 ms，两项相加约 1.7 ms）。
  //
  // 等价性（为什么可以直接不画）：画布刚被 `clear` 填满**不透明**底色 `B`，此时
  // `src-over` 一份同色不透明 `B` 得到的就是 `B` 本身（`over(B, B) == B`）。
  // 四个前提缺一不可，全在这个条件里：
  //   ① `cleared_fresh_`：清屏后**还没有任何绘制**碰过画布（否则那些像素已被改变）；
  //   ② 纯色不透明、`opacity >= 0.999`、`SrcOver`（其他情形不是替换语义）；
  //   ③ 颜色与 `clear` 的完全一致；
  //   ④ 覆盖**整张画布**（按物理像素比，不是逻辑坐标）且无圆角、无遮罩裁剪。
  const math::Color fill_color = paint.color();
  const math::IntRect whole{0, 0, physical_width_, physical_height_};
  if (cleared_fresh_ && skip_redundant_clear_fill_ && paint.is_solid() && fill_color.a == 255U &&
      radius <= 0.0f && options.opacity >= 0.999f && options.blend == BlendMode::SrcOver &&
      !has_mask_clip() && fill_color.r == cleared_color_.r &&
      fill_color.g == cleared_color_.g && fill_color.b == cleared_color_.b &&
      clipped.x <= whole.x && clipped.y <= whole.y && clipped.right() >= whole.right() &&
      clipped.bottom() >= whole.bottom()) {
    // 像素已经是目标值，不需再写；但“画布被碰过”的状态要跟着变（后续不能再跳）。
    cleared_fresh_ = false;
    return;
  }
  cleared_fresh_ = false;

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
  // ⚠ **从这里开始 `rect`/`radius`/`path` 都是物理像素**（本函数开头已乘过一次 `scale_`），
  // 因此全部往后的光栅化入口必须是 `rasterize_path_runs`（接物理坐标路径），
  // **不能是 `fill_path`**——后者内部还会 `path.scaled(scale_)`，会把已缩放的路径再缩一次。
  // 圆角矩形同样遵循这条：只在半径>0 时才会走到这里，而双重缩放会在
  // 细长条（高远小于宽）上把位置与宽度都算错。
  // **圆角矩形也走几何 sprite 缓存**。
  //
  // 为何值得：实测它是界面里最贵的一项（gallery 1600×1000 一帧 32 次 / 2.34 ms / 40%），
  // 而它的几何（宽/高/圆角半径）在帧间**完全不变**（卡片、面板、按钮就那几种尺寸）。
  // 每帧重新 `flatten` 4 条贝塞尔 → 76 条边 → 4× 子采样 → merge_runs 是纯重生。
  // 实测：1600×1000 整帧 5.78 → **5.15 ms**（round_rect 2.34 → 1.62 ms）。
  //
  // 为何只给 `radius > 0` 走：`radius == 0` 的直角矩形已有更快的整行 `blend_span` 快路径
  // （上面那条 `simple` 分支），不需要贴图。
  //
  // 注意这不是“用贴图代替矢量”——是“同一个矢量形状重复画时只光栅化一次”，
  // 任意缩放/DPI 仍由 `rect`/`radius` 进键区分（不同尺寸 = 不同条目）。
    if (radius > 0.0f) {
    // **先试几何贴图缓存（整块）**。
    //
    // 为何值得（实测 `tools/round_rect_route_probe.cpp`，1600×1000）：
    // 圆角的几何（宽/高/圆角半径）在帧间完全不变，而每帧都重光栅化：
    //
    //   形状                 不走缓存     贴图命中     倍数
    //   按钮 96×32 r6        0.017 ms     0.002 ms    6.88×
    //   卡片 320×160 r12     0.043 ms     0.015 ms    2.86×
    //   面板 720×480 r16     0.129 ms     0.118 ms    1.10×
    //
    // 小控件快近 7 倍——而它们正是界面里最多的形状（gallery 一帧 33 次圆角）。
    //
    // **等价性**：贴图内容就是“整块路径光栅化一次”。
    // `tools/round_rect_diff_locate_probe.cpp` 在 8 个档位（非整坐标/非整尺寸/
    // 半透明/极扁/胶囊/超画布）上逐像素对比：7 档**零差**，1 档在圆角弧最外缘
    // 差 **1 LSB**（贴图 8 位量化的固有代价，与三带拆分两侧相同）。
    //
    // 缓存未命中时 `try_geometry_sprite` 会**自己完成绘制**（建图并贴出），
    // 所以命中/未命中都是一次交付。
    if (fill_physical_path_with_cache(path, path, paint, options)) return;

    // **缓存不适用时（渐变画笔 / 遮罩裁剪 / 非常规混合）退整块光栅化**。
    //
    // 这里曾经是三带拆分（中带直角 + 上带 + 下带），好处是中带那片直边区
    // 可以走整行快路径（实测 1600×1000 从 0.562 → 0.333 ms）。但它有两个代价：
    //   ① **同一整块圆角路径被光栅化两遍**（上带与下带各一次，只是裁剪不同）；
    //   ② 分界必须对齐整像素，否则裁剪区重叠、那一行被画两次（半透明变深）。
    // 现在圆角矩形**绝大多数时候走贴图缓存**（上面那段），三带只在“形状不适合缓存”
    // 时接手——而那些场景（渐变/遮罩）本来就不该再假设形状会重复，
    // 多一次光栅化比多一套 90 行的三带拆分更简单可靠。
    rasterize_path_runs(path, paint, options);
    return;
  }
  rasterize_path_runs(path, paint, options);
  (void)logical_rect;
}

/// 把路径覆盖率写入画布（`fill_path` / `fill_circle` / 描边等所有路径绘制的共同出口）。
///
/// 为何包一层而不是各自直接调 `detail::fill_path_aa`：
/// `clear` 后的“画布还没被碰过”标记必须**在任何一次像素写入时失效**，否则
/// “跳过重复铺底”会把已经画上去的内容当作不存在。实测：`fill_circle` 直接调
/// 内部函数、绕过了 `fill_path`，于是“先画圆再铺同色底”两块输出差 **2040 像素**。
void Canvas::rasterize_path_runs(const Path& path, const Paint& paint, DrawOptions options) {
  cleared_fresh_ = false;
  detail::fill_path_aa(*this, path, paint, options);
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
  rasterize_path_runs(path, paint, options);
}

void Canvas::stroke_path(const Path& path, const Paint& paint, float width,
                         const StrokeStyle& style, DrawOptions options) {
  OpScope scope(*this, PaintOp::Stroke);
  cleared_fresh_ = false;
  if (width <= 0.0f || path.is_empty()) return;
  // DPI：描边宽度同比例放大，1px 发丝线在 2x 屏上是 2 物理像素（视觉等宽且更锐利）
  const float physical_width_value = width * scale_;
  const float tolerance = options.antialias ? 0.25f : 0.5f;
  const Path scaled = scale_ == 1.0f ? path : path.scaled(scale_);
  // **缓存关闭时完全不算键**（同 `fill_path` 的理由）。
  if (geometry_cache_enabled_ && geometry_sprite_cache_enabled()) {
    // **键建在源路径上**（见 `try_geometry_sprite`）：描边最贵的
    // `stroke_to_path`（扁平化 + 每段四边形 + 顶点补圆）因此也能被缓存跳过。
    // 平移原点按**描边扩展量**向外留边（半宽 + 2px），否则贴图会切掉帽/圆角。
    const float pad = physical_width_value * 0.5f + 2.0f;
    const math::Point origin = path_control_origin(path, scale_, pad);
    const std::uint64_t key = path_geometry_key(
        path, scale_,
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(physical_width_value * 1000.0f))
         << 8U) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(style.cap)) |
             (static_cast<std::uint64_t>(static_cast<std::uint8_t>(style.join)) << 2U)) |
            static_cast<std::uint64_t>(tolerance * 100000.0f) << 32U,
        origin);
    const auto build_sprite_path = [&scaled, origin, physical_width_value, tolerance,
                                    &style]() -> Path {
      return detail::stroke_to_path(scaled, physical_width_value, tolerance, style)
          .translated(-origin.x, -origin.y);
    };
    if (try_geometry_sprite(key, paint, options.opacity, options.blend, build_sprite_path, origin))
      return;
  }
  rasterize_path_runs(detail::stroke_to_path(scaled, physical_width_value, tolerance, style),
                      paint, options);
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

    if (!masked && options.blend == BlendMode::SrcOver && shadow_run_grouping_enabled_) {
      // **按同 α 游程分组**：长游程走 `simd::blend_row`（AVX2 8px/次），短游程逐像素。
      //
      // 为何有效（本轮实测，`tools/shadow_blend_route_probe.cpp`）：
      //   A 逐像素                    3.915 ms
      //   B 按段分组 + blend_row      2.181 ms   → **1.79×**，且**逐像素完全相同**。
      //
      // 为何不能“整行一次 SIMD”：阴影的 α 每个像素都可能不同（模糊出的梯度），
      // 而 `blend_row` 的语义是“整段同一个 α”。按游程切开就同时得到了两者。
      //
      // 为何阈值取 8：`blend_row` 的 AVX2 主循环一次吃 8 像素；少于 8 时
      // 它退化成“常量构造 + 标量尾巴”，不如保持逐像素。
      //
      // 数据支撑（`tools/shadow_run_length_probe.cpp`）：同 α 游程**中位长度 1**，
      // 但 **≥8px 的游程覆盖 80~85% 的像素**——即“少数长游程 + 大量孤点”，
      // 正是分组策略的适用形态。
      constexpr int kRunSimdThreshold = 8;
      int x = x_begin;
      while (x < x_end) {
        const std::uint8_t mask_byte = mask_row[x - mask_origin.x];
        int run_end = x + 1;
        while (run_end < x_end && mask_row[run_end - mask_origin.x] == mask_byte) ++run_end;
        const int run = run_end - x;
        if (mask_byte == 0U) {
          x = run_end;   // 全透明：整段跳过（比逐像素 branch 省）
          continue;
        }
        if (run >= kRunSimdThreshold) {
          // `scaled_sources` 已经把“遮罩值 → 预乘源色”算好，所以这里 alpha 传 255
          // （不再额外缩放），与逐像素的 `over_premul(row, scaled_sources[m])` 同式。
          simd::blend_row(row + x, static_cast<std::size_t>(run), scaled_sources[mask_byte], 255U);
        } else {
          const std::uint32_t source = scaled_sources[mask_byte];
          for (int index = x; index < run_end; ++index) {
            row[index] = over_premul(row[index], source);
          }
        }
        x = run_end;
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

  // **将被随后铺上的不透明区域**（`options.covered_region`）：落在其中的贴图像素
  // 最终会被不透明底色完全盖住，先合成一遍是纯浪费。
  //
  // 为何这是硬保证而非近似（`src-over` 结合律）：设贴图贡献 U、底色 B（`B.a == 255`，
  // 即不透明预乘），则 `over(B, over(D, U)) == B` 对任意 D、U 成立——
  // 因为不透明源的 `over` 就是**完全替换**。所以「跳过」与「先画再盖」逐像素相等。
  // 三个前提（任一不成立就**不能**跳，故条件全在 `plain` 分支上）：
  //   ① `blend == SrcOver`（其他模式不是替换语义）；
  //   ② `opacity >= 0.999`（半透明合成后底色仍半透明）；
  //   ③ 调用方保证覆盖区**之后真的**会被不透明底填满（`DrawOptions::covered_region` 的契约）。
  const math::IntRect covered_rect =
      plain ? to_physical(options.covered_region).intersect(clip) : math::IntRect{};
  // 向内缩 1 像素：几何边缘所在的那一像素只被抗锯齿**部分**覆盖，不能当成“确定被盖”。
  const math::IntRect covered_inner =
      covered_rect.is_empty() ? math::IntRect{}
                              : covered_rect.inflate(-1).intersect(clip);
  const float covered_radius =
      plain ? options.covered_radius * (scale_ > 0.0f ? scale_ : 1.0f) : 0.0f;

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
    int x_begin = std::max(origin_x + first, clip.x);
    int x_end = std::min(origin_x + last + 1, clip.right());
    if (x_begin >= x_end) continue;
    if (plain) {
      // 分段贴：把本行切成「确定不被覆盖」的若干段（被盖区左/右），段内走 SIMD
      // 逐像素 `src-over`。覆盖区内的像素会被随后铺上的不透明底色替换，
      // 跳过与不跳过逐像素等价（见上）。
      //
      // ⚠ **分段要看“跳过的量够不够本”**（2026-10-20 实测，`tools/simd_short_row_probe.cpp`）。
      //
      // 直接测「整行一次」vs「切两段跳过中间」（140px 宽，两侧均分剩余，多轮取最小）：
      //
      // | 跳过 | 两侧各 | 收益 |
      // |---|---|---|
      // | 40px | 50px | **+16~18%** |
      // | 80px | 30px | **−6~−10%** |
      // | 120px | 10px | **+55~62%** |
      //
      // 非单调：两侧 30px 反而最差（落在 `over_row` 向量主循环的部分迭代区间，
      // 尾巴走标量）；两侧 10px 很快（数据量本身就小）。单价分布（同探针）：
      // 长 1~6 → 3.6~6.1 ns/px、长 8 → 0.85、长 16 → 0.47、长 ≥32 → 0.28~0.33。
      //
      // 因此判据用**跳过量**而不是段长：跳过越多总是越便宜的，但少于 ~40px 时
      // 省下的混合抵不过多一次调用的开销。
      constexpr int kMinSkipPixels = 40;
      if (!covered_inner.is_empty()) {
        const CoveredSpan span = covered_span_at(covered_inner, covered_radius, y);
        if (!span.empty()) {
          const int cut_begin = std::max(x_begin, span.begin);
          const int cut_end = std::min(x_end, span.end);
          const int left_len = cut_begin - x_begin;
          const int right_len = x_end - cut_end;
          const int skipped = cut_end - cut_begin;
          if (skipped >= kMinSkipPixels) {
            if (left_len > 0) {
              simd::over_row(dst + x_begin, src + (x_begin - origin_x),
                             static_cast<std::size_t>(left_len));
            }
            if (right_len > 0) {
              simd::over_row(dst + cut_end, src + (cut_end - origin_x),
                             static_cast<std::size_t>(right_len));
            }
            continue;
          }
        }
      }
      simd::over_row(dst + x_begin, src + (x_begin - origin_x),
                     static_cast<std::size_t>(x_end - x_begin));
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

void Canvas::clear_geometry_sprites() { geometry_sprites_.clear(); }

auto Canvas::geometry_stats() const noexcept -> GeometryStats {
  GeometryStats stats = geometry_stats_;
  stats.entries = geometry_sprites_.size();
  return stats;
}

auto Canvas::path_control_origin(const Path& path, float scale, float pad) -> math::Point {
  // 取**控制点**包围盒的左上角（不扁平化：扁平化就是被测成本的一部分），
  // 向外留 `pad` 再**取整**（不是 floor）——
  //
  // 为何取整而不是 floor：`origin` 既是贴图的平移基准，也是“把图案搬到整像素网格上”的
  // 那个位移。贴图只能做**整像素**平移，所以它必须存的是“图案落在整像素原点时的样子”；
  // 若用 floor，贴图就会带着一个固定的 0~1 像素小数偏置，而那个偏置只对**首次**绘制点
  // 正确——换个位置（小数部分不同）抗锯齿就不同了（实测：522 像素差，最大通道差 255）。
  bool has = false;
  float min_x = 0.0f;
  float min_y = 0.0f;
  for (const auto& command : path.commands()) {
    const math::Point points[3] = {command.p1, command.p2, command.p3};
    const int count = command.kind == PathCommand::Kind::CubicTo   ? 3
                      : command.kind == PathCommand::Kind::QuadTo ? 2
                      : command.kind == PathCommand::Kind::Close  ? 0
                                                                  : 1;
    for (int index = 0; index < count; ++index) {
      if (!has) {
        min_x = points[index].x;
        min_y = points[index].y;
        has = true;
        continue;
      }
      min_x = std::min(min_x, points[index].x);
      min_y = std::min(min_y, points[index].y);
    }
  }
  if (!has) return math::Point{0.0f, 0.0f};
  return math::Point{std::round(min_x * scale - pad), std::round(min_y * scale - pad)};
}

auto Canvas::path_geometry_key(const Path& path, float scale, std::uint64_t extra,
                               math::Point origin) -> std::uint64_t {
  // 取**命令流控制点**（不扁平化：扁平化本身就是被测成本的一部分），
  // 且一律混入**相对 `origin` 的坐标**（不是绝对坐标）。
  //
  // 为何必须是相对坐标：贴图的全部意义就是“同一形状换个位置复用”。
  // 若键里带绝对坐标，平移就会变成一个新条目——缓存退化成“同一位置重复画时才命中”，
  // 而那恰恰是现实中最不可能发生的场景。
  //
  // ⚠ 先写的那版就是绝对的，后果很隐蔽：「换位置必须重新建图」那条判据会**恒为真**
  // （平移就改变几何键），于是相位漏进键的缺陷也照样判绿。
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix64 = [&hash](std::uint64_t part) {
    hash ^= part;
    hash *= 1099511628211ULL;
  };
  const auto quantize = [&mix64, origin](float value, float offset) {
    mix64(static_cast<std::uint64_t>(
        static_cast<std::int64_t>(std::lround((value - offset) * 256.0f)) + 0x40000000));
  };
  mix64(extra);
  mix64(static_cast<std::uint64_t>(static_cast<std::uint32_t>(scale * 100000.0f)));
  const auto commands = path.commands();
  mix64(commands.size());
  for (const auto& command : commands) {
    mix64(static_cast<std::uint64_t>(static_cast<std::uint8_t>(command.kind)));
    quantize(command.p1.x, origin.x);
    quantize(command.p1.y, origin.y);
    // 只有曲线才有 p2/p3；直线/移动只混 p1——否则“同一线段但残留控制点不同”
    // 会得到不同的键（同一条几何变成两个缓存条目）。
    const bool has_p2 = command.kind == PathCommand::Kind::QuadTo ||
                        command.kind == PathCommand::Kind::CubicTo;
    if (has_p2) {
      quantize(command.p2.x, origin.x);
      quantize(command.p2.y, origin.y);
    }
    if (command.kind == PathCommand::Kind::CubicTo) {
      quantize(command.p3.x, origin.x);
      quantize(command.p3.y, origin.y);
    }
  }
  return hash;
}

auto Canvas::try_geometry_sprite(std::uint64_t geometry_key, const Paint& paint, float opacity,
                                 BlendMode blend, const std::function<Path()>& build,
                                 math::Point draw_origin) -> bool {
  // ① 硬条件（不满足就老实走光栅化）
  if (!geometry_cache_enabled_ || !geometry_sprite_cache_enabled()) {
    ++geometry_stats_.rejected;
    return false;
  }
  if (!paint.is_solid() || (blend != BlendMode::SrcOver && blend != BlendMode::Src) ||
      has_mask_clip() || paint.color().a == 0U || opacity <= 0.0f) {
    ++geometry_stats_.rejected;
    return false;
  }
  const math::Color color = paint.color();

  // ② 完整键 = 几何 × 颜色 × 不透明度 × 混合模式
  std::uint64_t hash = geometry_key;
  const auto mix64 = [&hash](std::uint64_t part) {
    hash ^= part;
    hash *= 1099511628211ULL;
  };
  mix64(static_cast<std::uint64_t>(color.r) | (static_cast<std::uint64_t>(color.g) << 8U) |
        (static_cast<std::uint64_t>(color.b) << 16U) | (static_cast<std::uint64_t>(color.a) << 24U));
  mix64(static_cast<std::uint64_t>(static_cast<std::uint32_t>(math::clamp01(opacity) * 100000.0f)));
  mix64(static_cast<std::uint64_t>(blend));

  // ③ 查缓存（命中就贴到**本次**位置——贴图与位置无关）
  for (const auto& entry : geometry_sprites_) {
    if (entry.key == hash) {
      ++geometry_stats_.hits;
      return blit_geometry_sprite(entry.sprite, draw_origin);
    }
  }

  // ④ 未命中：先建几何（`build` 可能是描边扩展或 DPI 缩放）
  ++geometry_stats_.misses;
  const Path physical = build();
  if (physical.is_empty()) return false;
  // 离屏画布的画笔：纯色、`opacity` 烘进**画布混合**（与其后真正的贴图合成同口径）。
  //
  // ⚠ 两个坑：
  // 1. **不能**把 opacity 乘进颜色的 alpha：那会把 `style.opacity` 与颜色自身的 alpha
  //    混为一谈，随后贴图时又乘一次——实测 523 像素差、最大通道差 166。
  // 2. 必须用 **SrcOver**（不是 `Src`）——即使在透明底上二者结果相同。
  //    原因在 `blend_coverage_runs`：只有 `SrcOver` 走“先按像素累加覆盖率、同一像素只混一次”
  //    的路径；`Src` 是覆盖语义，同一像素被多段覆盖时只保留**最后一段**——
  //    圆角弧段正是“一像素多段”的典型（实测覆盖率 48/109 vs 正确值 88/164）。
  const raster::Paint sprite_paint = raster::Paint::solid(color);
  const raster::DrawOptions options_for_sprite{.blend = raster::BlendMode::SrcOver,
                                              .opacity = opacity};
  const auto polylines = physical.flatten(0.25f);
  if (polylines.empty()) return false;
  bool has_point = false;
  float min_x = 0.0f;
  float min_y = 0.0f;
  float max_x = 0.0f;
  float max_y = 0.0f;
  for (const auto& polyline : polylines) {
    for (const math::Point& point : polyline.points) {
      if (!has_point) {
        min_x = max_x = point.x;
        min_y = max_y = point.y;
        has_point = true;
        continue;
      }
      min_x = std::min(min_x, point.x);
      min_y = std::min(min_y, point.y);
      max_x = std::max(max_x, point.x);
      max_y = std::max(max_y, point.y);
    }
  }
  if (!has_point) return false;
  const math::Rect bounds = math::Rect::from_ltrb(min_x, min_y, max_x, max_y);
  if (bounds.is_empty()) return false;
  const math::IntRect area = bounds.round_out().inflate(1);
  if (area.is_empty()) return false;
  const auto pixel_count = static_cast<long long>(area.width) * static_cast<long long>(area.height);
  // 面积超限：不建贴图（大面积图案建图比画一次贵，且缓存会持续吃内存）。
  if (pixel_count <= 0 || pixel_count > kGeometrySpriteMaxPixels) return false;

  // **用原管线在离屏画布上画一遍**，而不是自己再实现一份像素交付：
  //
  // 两个方案都可行，但只有这个的等价性是**由构造保证**的：贴图内容就是
  // `fill_path_aa` 在一张透明底画布上的输出，于是“贴图”与“直接画”是同一份代码画出的
  // 同一批像素（后者只是多一层 `src-over` 到已有背景上，结合律）。
  // 实测教训：先写的那版自己重算了覆盖率（`rasterize_path_alpha`），
  // 结果在两套量化/坐标口径上悄悄漂了 238 个像素（一整块 18×20 错位）。
  raster::Canvas scratch(area.width, area.height, 1.0f);
  detail::fill_path_aa(scratch,
                       physical.translated(-static_cast<float>(area.x),
                                           -static_cast<float>(area.y)),
                       sprite_paint, options_for_sprite);
  const auto scratch_pixels = scratch.pixels();
  std::vector<std::uint32_t> baked(scratch_pixels.begin(), scratch_pixels.end());
  std::vector<std::pair<int, int>> spans(static_cast<std::size_t>(area.height), {-1, -1});
  bool any = false;
  for (int row = 0; row < area.height; ++row) {
    const auto* line =
        baked.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(area.width);
    int first = -1;
    int last = -1;
    for (int x = 0; x < area.width; ++x) {
      if ((line[x] & 0xFFU) == 0U) continue;
      if (first < 0) first = x;
      last = x;
    }
    if (first >= 0) {
      spans[static_cast<std::size_t>(row)] = {first, last};
      any = true;
    }
  }
  if (!any) return false;

  GeometrySprite sprite;
  sprite.pixels = std::make_shared<const std::vector<std::uint32_t>>(std::move(baked));
  sprite.spans = std::make_shared<const std::vector<std::pair<int, int>>>(std::move(spans));
  // `build` 返回的已是**相对 origin 平移后的**路径，因此 `area` 就是相对绘制点的偏移。
  sprite.x = area.x;
  sprite.y = area.y;
  sprite.width = area.width;
  sprite.height = area.height;
  if (geometry_sprites_.size() >= kGeometrySpriteLimit) {
    geometry_sprites_.erase(geometry_sprites_.begin());
  }
  geometry_sprites_.push_back(GeometryCacheEntry{hash, std::move(sprite)});
  return blit_geometry_sprite(geometry_sprites_.back().sprite, draw_origin);
}

auto Canvas::blit_geometry_sprite(const GeometrySprite& sprite, math::Point origin) -> bool {
  // 颜色与不透明度已烘进贴图（键里含它们），这里只负责把预乘色按 `src-over` 贴上去。
  cleared_fresh_ = false;
  const math::IntRect clip = clip_rect();
  if (clip.is_empty() || sprite.pixels == nullptr || sprite.spans == nullptr) return true;
  // `origin` 是**物理像素、整数**（由 `path_control_origin` 产出），
  // 因此贴图与直接光栅化落在同一像素相位上。
  const int origin_x = static_cast<int>(std::lround(origin.x)) + sprite.x;
  const int origin_y = static_cast<int>(std::lround(origin.y)) + sprite.y;
  const auto& pixels = *sprite.pixels;
  const auto& spans = *sprite.spans;
  for (int row = 0; row < sprite.height; ++row) {
    const int y = origin_y + row;
    if (y < clip.y || y >= clip.bottom()) continue;
    const auto [first, last] = spans[static_cast<std::size_t>(row)];
    if (first < 0) continue;
    const int x_begin = std::max(origin_x + first, clip.x);
    const int x_end = std::min(origin_x + last + 1, clip.right());
    if (x_begin >= x_end) continue;
    const auto* src = pixels.data() + static_cast<std::size_t>(row) *
                                        static_cast<std::size_t>(sprite.width);
    auto* dst = pixels_.data() + static_cast<std::size_t>(y) *
                                     static_cast<std::size_t>(physical_width_);
    simd::over_row(dst + x_begin, src + (x_begin - origin_x),
                   static_cast<std::size_t>(x_end - x_begin));
  }
  return true;
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
      // ⚠ **必须传 `options.blend`**（不能硬编码 `over_premul`）：这是图层合成的
      // 唯一入口，而“每个图层带自己的混合模式”正是图像编辑器的核心能力。
      // 实测（鈥月示例的第一版）：本行原先写死 SrcOver，于是九种混合模式里
      // **八种退化成普通叠加**——界面上看不出异常（层不透明度生效、层顺序也对），
      // 只有把合成结果导出后逐像素比对，才能发现“乘算结果等于直接覆盖”。
      //
      // `coverage` 取的是**源 alpha**（预乘缓冲的第 0 通道）：双线性插值后
      // 它就是逐像素覆盖率，与 `draw_mask` 同一口径（那里也是这么传的）。
      row[x] = blend_pixel_premul(row[x], blended, coverage, options.blend);
    }
  }
}

void Canvas::draw_canvas_at(const Surface& source, int x, int y, DrawOptions options) {
  OpScope scope(*this, PaintOp::Image);
  // ⚠ `x/y` 是**物理像素**（函数名与参数类型都是“像素级贴图”），而 `draw_canvas` 的
  // `destination` 是**逻辑坐标**（内部还会 `to_physical`）——直接传会把 x/y 再乘一次
  // `device_scale`，内容整体**向右下溢出**。
  //
  // 实测（用户报“标题栏 logo 位置偏下”的根因）：DPI 1.5 下贴到物理 (18,18) 的内容
  // 落在 27..50——偏了 9 物理（= 18/2），看着像“图标没居中”，实际是坐标系混用。
  // 回归：`tests/raster_canvas_at_scale_test.cpp`。
  const float target_scale = scale_ > 0.0f ? scale_ : 1.0f;
  draw_canvas(source,
              math::Rect{static_cast<float>(x) / target_scale,
                         static_cast<float>(y) / target_scale,
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
