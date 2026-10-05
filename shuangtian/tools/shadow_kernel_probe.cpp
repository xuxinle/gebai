// 阴影合成核（kernel）对照探针（性能轮）。
//
// ## 为什么从这个角度切
//
// 逐原语分解显示卡片两层投影占整帧 paint 的 ~58%（14 ms / 96 张）。遮罩本身**有缓存**
// （`shadow_mask` 按几何参数缓存），所以稳态成本全在**合成循环**上：对 region 内每个像素
// 做一次 `over_premul(表查值)`。
//
// 本探针用**自己的像素缓冲**复刻 `Canvas::draw_shadow` 的合成循环（读同一份遮罩构造：
// `detail::rasterize_mask` + `blur_mask`），逐条量候选优化的收益与像素代价：
//
//   A 现状           ：region = rect.inflate(blur*2+2)，整矩形跑
//   B 收紧 region    ：blur 的真实支撑 = 三次盒式模糊 3·round(blur/2)（+2 安全量）
//   C = B + 行跨度   ：每行只跑「首个非零 .. 末个非零」
//   D = C + 跳过被盖区：跳掉**卡片不透明底色将完全盖住**的像素（内缩 1px 的圆角矩形）
//
// D 的前提是"调用方随后会用自己的不透明底色铺满这个圆角矩形"——`Element::paint_box`
// 的绘制序正是如此（先阴影、后背景），因此对被完全覆盖的像素，这笔混合对最终画面
// 没有贡献。它不是近似。
//
// 用法： shadow_kernel_probe [repeat] [blur] [w] [h] [radius]

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "rasterize_internal.hpp"

#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/theme.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline constexpr int kCanvasW = 1280;
inline constexpr int kCanvasH = 800;

// 与 canvas.cpp 同一份语义（复制以便量"改这里有没有用"）。
[[nodiscard]] constexpr auto channel(std::uint32_t pixel, unsigned shift) noexcept -> std::uint32_t {
  return (pixel >> shift) & 0xFFU;
}
[[nodiscard]] constexpr auto pack(std::uint32_t r, std::uint32_t g, std::uint32_t b,
                                  std::uint32_t a) noexcept -> std::uint32_t {
  return (r << 24U) | (g << 16U) | (b << 8U) | a;
}
[[nodiscard]] constexpr auto fast_div255(std::uint32_t value) noexcept -> std::uint32_t {
  return (value * 0x8081U) >> 23U;
}
[[nodiscard]] constexpr auto scale_premul(std::uint32_t src, std::uint32_t alpha) noexcept
    -> std::uint32_t {
  const auto scale = [alpha](std::uint32_t value) constexpr noexcept -> std::uint32_t {
    return fast_div255(value * alpha + 127U);
  };
  return pack(scale(channel(src, 24)), scale(channel(src, 16)), scale(channel(src, 8)),
              scale(channel(src, 0)));
}
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

struct Buffer {
  int width{kCanvasW};
  int height{kCanvasH};
  std::vector<std::uint32_t> pixels;

  explicit Buffer(std::uint32_t fill) : pixels(static_cast<std::size_t>(width) * height, fill) {}
  void clear(std::uint32_t fill) { std::fill(pixels.begin(), pixels.end(), fill); }
  [[nodiscard]] auto pixel(int x, int y) const noexcept -> std::uint32_t {
    return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                  static_cast<std::size_t>(x)];
  }
};

struct Layer {
  std::shared_ptr<st::raster::Mask> mask;
  st::math::IntRect region;
};

/// 建一层阴影遮罩。`box` 为卡片矩形（**画布绝对坐标**）；遮罩本身在**局部坐标**栅格化
/// （位置无关，实现里就是这样跨元素复用的），落位由 `box + offset + padding` 算出。
[[nodiscard]] auto build_mask(st::math::Rect box, float radius, float blur, st::math::Point offset,
                              float padding) -> Layer {
  const st::math::Rect region = box.offset(offset.x, offset.y).inflate(padding);
  const st::math::IntRect region_int = region.round_out();
  auto mask = std::make_shared<st::raster::Mask>(region_int.width, region_int.height);
  const st::math::Rect local{offset.x - static_cast<float>(region_int.x) + box.x,
                             offset.y - static_cast<float>(region_int.y) + box.y, box.width,
                             box.height};
  st::raster::detail::rasterize_mask(*mask, st::raster::make_rounded_rect(local, radius), 0.0f,
                                     0.0f);
  st::raster::blur_mask(*mask, blur);
  return Layer{mask, region_int};
}

enum class Mode { A_Current, B_TightRegion, C_RowSpan, D_Covered };

struct Covered {
  st::math::Rect rect;
  float radius{12.0f};

  [[nodiscard]] auto contains(st::math::Point point) const -> bool {
    if (point.x < rect.x || point.x >= rect.right() || point.y < rect.y ||
        point.y >= rect.bottom()) {
      return false;
    }
    const float r = radius;
    if (r <= 0.0f) return true;
    const float cx = point.x < rect.x + r ? rect.x + r
                                          : (point.x > rect.right() - r ? rect.right() - r : point.x);
    const float cy = point.y < rect.y + r ? rect.y + r
                                          : (point.y > rect.bottom() - r ? rect.bottom() - r : point.y);
    if (cx == point.x && cy == point.y) return true;
    const float dx = point.x - cx;
    const float dy = point.y - cy;
    return dx * dx + dy * dy <= r * r;
  }
};

/// 合成一层阴影到缓冲；返回实际混合的像素数。
auto composite(Buffer& buffer, const Layer& layer, st::math::Color color, float opacity, Mode mode,
               const Covered* covered, std::vector<std::pair<int, int>>& spans,
               bool build_spans_flag) -> std::size_t {
  const std::uint32_t premul = st::math::premultiply(color);
  std::array<std::uint32_t, 256> scaled{};
  for (std::size_t level = 0; level < scaled.size(); ++level) {
    const float normalized = static_cast<float>(level) / 255.0f * opacity;
    scaled[level] = scale_premul(
        premul, static_cast<std::uint32_t>(st::math::clamp01(normalized) * 255.0f + 0.5f));
  }
  const auto values = layer.mask->values();
  const int mask_w = layer.mask->width();
  const int mask_h = layer.mask->height();
  if (build_spans_flag) {
    spans.assign(static_cast<std::size_t>(mask_h), {-1, -1});
    for (int y = 0; y < mask_h; ++y) {
      const std::uint8_t* row =
          values.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(mask_w);
      int first = -1;
      int last = -1;
      for (int x = 0; x < mask_w; ++x) {
        if (row[x] == 0U) continue;
        if (first < 0) first = x;
        last = x;
      }
      spans[static_cast<std::size_t>(y)] = {first, last};
    }
  }

  const int area_x = std::max(0, layer.region.x);
  const int area_y = std::max(0, layer.region.y);
  const int area_right = std::min(buffer.width, layer.region.x + layer.region.width);
  const int area_bottom = std::min(buffer.height, layer.region.y + layer.region.height);
  std::size_t touched = 0;
  for (int y = area_y; y < area_bottom; ++y) {
    const int mask_y = y - layer.region.y;
    if (mask_y < 0 || mask_y >= mask_h) continue;
    const std::uint8_t* mask_row =
        values.data() + static_cast<std::size_t>(mask_y) * static_cast<std::size_t>(mask_w);
    int x_begin = area_x;
    int x_end = area_right;
    if (mode == Mode::C_RowSpan || mode == Mode::D_Covered) {
      const auto& span = spans[static_cast<std::size_t>(mask_y)];
      if (span.first < 0) continue;
      x_begin = std::max(x_begin, layer.region.x + span.first);
      x_end = std::min(x_end, layer.region.x + span.second + 1);
    }
    auto* row = buffer.pixels.data() + static_cast<std::size_t>(y) *
                                           static_cast<std::size_t>(buffer.width);
    for (int x = x_begin; x < x_end; ++x) {
      if (mode == Mode::D_Covered && covered != nullptr &&
          covered->contains(st::math::Point{static_cast<float>(x) + 0.5f,
                                            static_cast<float>(y) + 0.5f})) {
        continue;
      }
      const std::uint8_t mask_byte = mask_row[x - layer.region.x];
      if (mask_byte == 0U) continue;
      ++touched;
      row[x] = over_premul(row[x], scaled[mask_byte]);
    }
  }
  return touched;
}

struct Diff {
  std::size_t differing{0};
  int max_delta{0};
  int wx{0};
  int wy{0};
};

auto diff_buffers(const Buffer& left, const Buffer& right) -> Diff {
  Diff out;
  for (int y = 0; y < left.height; ++y) {
    for (int x = 0; x < left.width; ++x) {
      const std::uint32_t a = left.pixel(x, y);
      const std::uint32_t b = right.pixel(x, y);
      int delta = 0;
      for (unsigned shift : {24U, 16U, 8U, 0U}) {
        delta = std::max(delta, std::abs(static_cast<int>(channel(a, shift)) -
                                         static_cast<int>(channel(b, shift))));
      }
      if (delta > 0) ++out.differing;
      if (delta > out.max_delta) {
        out.max_delta = delta;
        out.wx = x;
        out.wy = y;
      }
    }
  }
  return out;
}

/// 把不透明底色铺进缓冲（模拟 `paint_box` 的下一步）。
auto fill_round_rect(Buffer& buffer, st::math::Rect rect, float radius, st::math::Color color)
    -> void {
  const std::uint32_t value = st::math::premultiply(color);
  for (int y = std::max(0, static_cast<int>(std::floor(rect.y)));
       y < std::min(buffer.height, static_cast<int>(std::ceil(rect.bottom()))); ++y) {
    for (int x = std::max(0, static_cast<int>(std::floor(rect.x)));
         x < std::min(buffer.width, static_cast<int>(std::ceil(rect.right()))); ++x) {
      const st::math::Point point{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
      bool inside = point.x >= rect.x && point.x < rect.right() && point.y >= rect.y &&
                    point.y < rect.bottom();
      if (inside && radius > 0.0f) {
        const float cx = point.x < rect.x + radius
                             ? rect.x + radius
                             : (point.x > rect.right() - radius ? rect.right() - radius : point.x);
        const float cy = point.y < rect.y + radius ? rect.y + radius
                                                   : (point.y > rect.bottom() - radius
                                                          ? rect.bottom() - radius
                                                          : point.y);
        if (cx != point.x || cy != point.y) {
          const float dx = point.x - cx;
          const float dy = point.y - cy;
          inside = dx * dx + dy * dy <= radius * radius;
        }
      }
      if (inside) buffer.pixels[static_cast<std::size_t>(y) *
                                    static_cast<std::size_t>(buffer.width) +
                                static_cast<std::size_t>(x)] = value;
    }
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const int repeat = argc > 1 ? std::atoi(argv[1]) : 300;
  const float blur = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 22.0f;
  const float card_w = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 140.0f;
  const float card_h = argc > 4 ? static_cast<float>(std::atof(argv[4])) : 48.0f;
  const float radius = argc > 5 ? static_cast<float>(std::atof(argv[5])) : 12.0f;

  const st::ui::Theme theme = st::ui::Theme::light();
  const st::math::Color color = theme.colors().shadow;
  const st::math::Point offset{0.0f, 2.0f};
  const st::math::Rect box{400.0f, 300.0f, card_w, card_h};
  const std::uint32_t background = st::math::premultiply(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
  const std::uint32_t surface = st::math::premultiply(st::math::Color::rgb(0xFF, 0xFF, 0xFF));

  const Layer current = build_mask(box, radius, blur, offset, blur * 2.0f + 2.0f);
  const float box_radius = static_cast<float>(std::lround(blur * 0.5f));
  const float tight_padding = std::max(2.0f, box_radius * 3.0f + 2.0f);
  const Layer tight = build_mask(box, radius, blur, offset, tight_padding);

  std::vector<std::pair<int, int>> spans;
  const Covered cover{box.inset(st::math::Insets::all(1.0f)), radius > 1.0f ? radius - 1.0f : 0.0f};

  st::print("阴影合成核 · blur={:.0f} · 卡片 {:.0f}×{:.0f} r={:.0f} · 偏移 y={:.0f}\n",
            static_cast<double>(blur), static_cast<double>(card_w), static_cast<double>(card_h),
            static_cast<double>(radius), static_cast<double>(offset.y));
  st::print("  region 现状 {:>4}×{:>4}={:>6} px · 收紧 {:>4}×{:>4}={:>6} px（padding {:.0f}→{:.0f}）\n",
            current.region.width, current.region.height,
            current.region.width * current.region.height, tight.region.width, tight.region.height,
            tight.region.width * tight.region.height, blur * 2.0f + 2.0f, tight_padding);
  {
    const auto values = tight.mask->values();
    std::size_t empty = 0;
    std::size_t full = 0;
    for (int y = 0; y < tight.mask->height(); ++y) {
      const auto* row =
          values.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(tight.mask->width());
      bool any = false;
      for (int x = 0; x < tight.mask->width(); ++x) {
        if (row[x] != 0U) {
          any = true;
          break;
        }
      }
      if (!any) ++empty;
    }
    std::size_t buckets[8]{};
    std::uint8_t max_value = 0;
    for (const std::uint8_t value : values) {
      ++buckets[static_cast<std::size_t>(value) / 32U];
      max_value = std::max(max_value, value);
    }
    st::print("  遮罩：空行 {:.0f}% · 最大值 {} · 分布 [0-31]={} [32-63]={} [64-95]={} [96-127]={} "
              "[128-159]={} [160-191]={} [192-223]={} [224-255]={}\n",
              100.0 * static_cast<double>(empty) / static_cast<double>(tight.mask->height()),
              static_cast<int>(max_value), buckets[0], buckets[1], buckets[2], buckets[3],
              buckets[4], buckets[5], buckets[6], buckets[7]);
    st::print("  遮罩 region 落位 x={} y={} w={} h={} · 卡片 {} {} {} {}\n", tight.region.x,
              tight.region.y, tight.region.width, tight.region.height,
              static_cast<double>(box.x), static_cast<double>(box.y),
              static_cast<double>(box.width), static_cast<double>(box.height));
  }

  struct Run {
    std::string name;
    Mode mode;
    const Layer* layer;
  };
  const std::vector<Run> runs{
      {"A 现状（原 region 整矩形）", Mode::A_Current, &current},
      {"B 收紧 region", Mode::B_TightRegion, &tight},
      {"C 收紧 + 行跨度", Mode::C_RowSpan, &tight},
      {"D 收紧 + 行跨度 + 跳过被盖区", Mode::D_Covered, &tight},
  };

  double reference = 0.0;
  for (const Run& run : runs) {
    Buffer buffer(background);
    std::size_t touched = 0;
    double best = 1e9;
    // 行跨度表**只建一次**（它只取决于遮罩，是可选优化的一部分，不该进计时循环）
    if (run.mode != Mode::A_Current) {
      composite(buffer, *run.layer, color, 1.0f, Mode::B_TightRegion, nullptr, spans, true);
    }
    for (int index = 0; index < repeat; ++index) {
      buffer.clear(background);
      const auto start = Clock::now();
      touched = composite(buffer, *run.layer, color, 1.0f, run.mode,
                          run.mode == Mode::D_Covered ? &cover : nullptr, spans, false);
      best = std::min(best,
                      std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    if (!reference) reference = best;
    st::print("  {:<30} {:>8.4} ms · 提速 {:>5.2}× · 混合 {:>6} px\n", run.name, best,
              reference / std::max(best, 1e-9), touched);
  }

  // 等价性：B 必须与 A 逐像素相同；D 的差必须在「铺上不透明底色后」全部消失。
  Buffer a_buffer(background);
  (void)composite(a_buffer, current, color, 1.0f, Mode::A_Current, nullptr, spans, false);
  Buffer b_buffer(background);
  (void)composite(b_buffer, tight, color, 1.0f, Mode::B_TightRegion, nullptr, spans, true);
  Buffer d_buffer(background);
  (void)composite(d_buffer, tight, color, 1.0f, Mode::D_Covered, &cover, spans, true);

  const Diff ab = diff_buffers(a_buffer, b_buffer);
  st::print("  A vs B：超差 {} px（最大Δ{}）——收紧 region 必须无损\n", ab.differing, ab.max_delta);
  const Diff db = diff_buffers(b_buffer, d_buffer);
  st::print("  B vs D：超差 {} px（最大Δ{}）——差异应只落在被盖子覆盖处\n", db.differing,
            db.max_delta);

  fill_round_rect(d_buffer, box, radius, st::math::Color::rgb(0xFF, 0xFF, 0xFF));
  fill_round_rect(a_buffer, box, radius, st::math::Color::rgb(0xFF, 0xFF, 0xFF));
  const Diff final_diff = diff_buffers(a_buffer, d_buffer);
  st::print("  铺底色后 D vs 现状：超差 {} px（最大Δ{}）← **最终画面等价性**\n", final_diff.differing,
            final_diff.max_delta);
  (void)surface;
  return 0;
}
