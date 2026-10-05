// 阴影结构方案对照（性能轮）：两层各自合成 vs 合并成一张「阴影贴图」一次贴。
//
// ## 动机
//
// `paint_box` 对每张卡片画**两层**投影（关键层 + 环境层），每层一次全区域像素合成。
// 96 张卡片 → 192 次合成 → 实测 14 ms（整帧 paint 的 58%）。
//
// 两层遮罩都**只取决于几何参数**（尺寸/圆角/模糊/偏移/颜色），与绘制位置无关——
// 现在实现已经把单层遮罩缓存住了（`shadow_mask`）。本探针比较：
//
//   A 现状       ：两个缓存遮罩，两次合成（每像素一次查表 + `over_premul`）
//   B 合并贴图   ：把两层**预先合成为一张预乘 RGBA 贴图**（缓存），每张卡片只贴一次；
//                  贴图按非零包围盒裁剪，逐行按行内非零跨度跑
//   C 合并贴图 + 跳过被盖区：再跳过卡片自身不透明底色盖住的区域
//
// 逐像素**等价性**以「铺上不透明底色后的画面」为准（那才是用户看到的）。

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

struct Layer {
  std::shared_ptr<st::raster::Mask> mask;
  st::math::IntRect region;
  st::math::Color color;
  float opacity{1.0f};
};

[[nodiscard]] auto build_layer(st::math::Rect box, float radius, float blur, st::math::Point offset,
                               float padding, st::math::Color color) -> Layer {
  const st::math::IntRect region_int = box.offset(offset.x, offset.y).inflate(padding).round_out();
  auto mask = std::make_shared<st::raster::Mask>(region_int.width, region_int.height);
  const st::math::Rect local{offset.x - static_cast<float>(region_int.x) + box.x,
                             offset.y - static_cast<float>(region_int.y) + box.y, box.width,
                             box.height};
  st::raster::detail::rasterize_mask(*mask, st::raster::make_rounded_rect(local, radius), 0.0f,
                                     0.0f);
  st::raster::blur_mask(*mask, blur);
  return Layer{mask, region_int, color, 1.0f};
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

/// 现状：逐层合成（与 `Canvas::draw_shadow` 同一算法与口径）。
auto composite_layer(Buffer& buffer, const Layer& layer) -> std::size_t {
  const std::uint32_t premul = st::math::premultiply(layer.color);
  std::array<std::uint32_t, 256> scaled{};
  for (std::size_t level = 0; level < scaled.size(); ++level) {
    const float normalized = static_cast<float>(level) / 255.0f * layer.opacity;
    scaled[level] = scale_premul(
        premul, static_cast<std::uint32_t>(st::math::clamp01(normalized) * 255.0f + 0.5f));
  }
  const auto values = layer.mask->values();
  const int mask_w = layer.mask->width();
  const int mask_h = layer.mask->height();
  const int x0 = std::max(0, layer.region.x);
  const int y0 = std::max(0, layer.region.y);
  const int x1 = std::min(buffer.width, layer.region.x + layer.region.width);
  const int y1 = std::min(buffer.height, layer.region.y + layer.region.height);
  std::size_t touched = 0;
  for (int y = y0; y < y1; ++y) {
    const int my = y - layer.region.y;
    if (my < 0 || my >= mask_h) continue;
    const std::uint8_t* mask_row =
        values.data() + static_cast<std::size_t>(my) * static_cast<std::size_t>(mask_w);
    auto* row = buffer.pixels.data() + static_cast<std::size_t>(y) *
                                           static_cast<std::size_t>(buffer.width);
    for (int x = x0; x < x1; ++x) {
      const std::uint8_t m = mask_row[x - layer.region.x];
      if (m == 0U) continue;
      ++touched;
      row[x] = over_premul(row[x], scaled[m]);
    }
  }
  return touched;
}

/// 合并贴图：两层预先合成成一张预乘 RGBA 贴图（只依赖几何 → 可跨元素缓存）。
struct Sprite {
  int x{0};
  int y{0};
  int width{0};
  int height{0};
  std::vector<std::uint32_t> pixels;              ///< 预乘，已含源色与不透明度
  std::vector<std::pair<int, int>> spans;         ///< 每行 [first, last]（-1 = 空行）
};

[[nodiscard]] auto build_sprite(const Layer& key, const Layer& env) -> Sprite {
  const int left = std::min(key.region.x, env.region.x);
  const int top = std::min(key.region.y, env.region.y);
  const int right = std::max(key.region.right(), env.region.right());
  const int bottom = std::max(key.region.bottom(), env.region.bottom());
  const int width = right - left;
  const int height = bottom - top;

  // ⚠ 贴图必须存**阴影自身的预乘 RGBA**（从全透明开始合成），而不是"合成到背景上的结果"——
  // 后者会把背景色烘进贴图，一旦元素底下是别的东西，贴上去就是错的。
  // 从透明开始做 src-over：src-over 满足结合律，所以"先合并两层、再贴到目标"
  // 与"两次依次贴到目标"**逐像素等价**（与目标底色无关）。
  Sprite sprite;
  sprite.x = left;
  sprite.y = top;
  sprite.width = width;
  sprite.height = height;
  sprite.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
  sprite.spans.assign(static_cast<std::size_t>(height), {-1, -1});

  const auto paint_layer = [&sprite, left, top, width, height](const Layer& layer) {
    const std::uint32_t premul = st::math::premultiply(layer.color);
    std::array<std::uint32_t, 256> scaled{};
    for (std::size_t level = 0; level < scaled.size(); ++level) {
      scaled[level] = scale_premul(
          premul, static_cast<std::uint32_t>(st::math::clamp01(
                                                 static_cast<float>(level) / 255.0f) * 255.0f + 0.5f));
    }
    const auto values = layer.mask->values();
    const int mask_w = layer.mask->width();
    const int mask_h = layer.mask->height();
    const int y_begin = std::max(top, layer.region.y);
    const int y_end = std::min(top + height, layer.region.y + layer.region.height);
    const int x_begin = std::max(left, layer.region.x);
    const int x_end = std::min(left + width, layer.region.x + layer.region.width);
    for (int y = y_begin; y < y_end; ++y) {
      const int my = y - layer.region.y;
      if (my < 0 || my >= mask_h) continue;
      const std::uint8_t* mask_row =
          values.data() + static_cast<std::size_t>(my) * static_cast<std::size_t>(mask_w);
      auto* row = sprite.pixels.data() + static_cast<std::size_t>(y - top) *
                                             static_cast<std::size_t>(width);
      for (int x = x_begin; x < x_end; ++x) {
        const std::uint8_t m = mask_row[x - layer.region.x];
        if (m == 0U) continue;
        row[x - left] = over_premul(row[x - left], scaled[m]);
      }
    }
  };
  paint_layer(env);   // 环境层先（大而淡）——与 paint_box 的顺序一致
  paint_layer(key);

  for (int row = 0; row < height; ++row) {
    const auto* src = sprite.pixels.data() + static_cast<std::size_t>(row) *
                                                  static_cast<std::size_t>(width);
    for (int column = 0; column < width; ++column) {
      if (src[column] == 0U) continue;
      if (sprite.spans[static_cast<std::size_t>(row)].first < 0) {
        sprite.spans[static_cast<std::size_t>(row)].first = column;
      }
      sprite.spans[static_cast<std::size_t>(row)].second = column;
    }
  }
  return sprite;
}

/// 贴图绘制：逐行按行内非零跨度做一次 src-over（贴图存的是**阴影自身的预乘 RGBA**）。
auto blit_sprite(Buffer& buffer, const Sprite& sprite, bool skip_covered,
                 const st::math::Rect* cover) -> std::size_t {
  std::size_t touched = 0;
  for (int row = 0; row < sprite.height; ++row) {
    const auto [first, last] = sprite.spans[static_cast<std::size_t>(row)];
    if (first < 0) continue;
    const int y = sprite.y + row;
    if (y < 0 || y >= buffer.height) continue;
    const auto* src = sprite.pixels.data() + static_cast<std::size_t>(row) *
                                                  static_cast<std::size_t>(sprite.width);
    auto* dst = buffer.pixels.data() + static_cast<std::size_t>(y) *
                                           static_cast<std::size_t>(buffer.width);
    for (int column = first; column <= last; ++column) {
      const int x = sprite.x + column;
      if (x < 0 || x >= buffer.width) continue;
      if (skip_covered && cover != nullptr &&
          static_cast<float>(x) + 0.5f >= cover->x &&
          static_cast<float>(x) + 0.5f < cover->right() &&
          static_cast<float>(y) + 0.5f >= cover->y &&
          static_cast<float>(y) + 0.5f < cover->bottom()) {
        continue;
      }
      ++touched;
      dst[x] = over_premul(dst[x], src[column]);
    }
  }
  return touched;
}

struct Diff {
  std::size_t differing{0};
  int max_delta{0};
};

auto diff_buffers(const Buffer& a, const Buffer& b) -> Diff {
  Diff out;
  for (int y = 0; y < a.height; ++y) {
    for (int x = 0; x < a.width; ++x) {
      const std::uint32_t lhs = a.pixel(x, y);
      const std::uint32_t rhs = b.pixel(x, y);
      int delta = 0;
      for (unsigned shift : {24U, 16U, 8U, 0U}) {
        delta = std::max(delta, std::abs(static_cast<int>(channel(lhs, shift)) -
                                         static_cast<int>(channel(rhs, shift))));
      }
      if (delta > 0) ++out.differing;
      out.max_delta = std::max(out.max_delta, delta);
    }
  }
  return out;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const int repeat = argc > 1 ? std::atoi(argv[1]) : 200;
  const float card_w = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 140.0f;
  const float card_h = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 48.0f;
  const float radius = argc > 4 ? static_cast<float>(std::atof(argv[4])) : 12.0f;

  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::Shadow md = st::ui::shadow_md(theme);
  const st::math::Rect box{400.0f, 300.0f, card_w, card_h};
  const std::uint32_t background = st::math::premultiply(st::math::Color::rgb(0xF7, 0xF8, 0xFA));

  // 现状：每层 padding = blur*2+2
  const Layer key = build_layer(box, radius, md.blur, st::math::Point{md.offset_x, md.offset_y},
                                md.blur * 2.0f + 2.0f, md.color);
  const Layer env = build_layer(box, radius, md.blur2, st::math::Point{md.offset2_x, md.offset2_y},
                                md.blur2 * 2.0f + 2.0f, md.color2);
  const Sprite sprite = build_sprite(key, env);

  std::size_t sprite_nonzero = 0;
  for (const auto& [first, last] : sprite.spans) {
    if (first >= 0) sprite_nonzero += static_cast<std::size_t>(last - first + 1);
  }

  st::print("阴影结构对照 · 卡片 {:.0f}×{:.0f} r={:.0f} · 关键层 blur={:.0f} 环境层 blur={:.0f}\n",
            static_cast<double>(card_w), static_cast<double>(card_h), static_cast<double>(radius),
            static_cast<double>(md.blur), static_cast<double>(md.blur2));
  st::print("  关键层 region {}×{} · 环境层 {}×{} · 贴图 {}×{}（非零跨度 {} px）\n",
            key.region.width, key.region.height, env.region.width, env.region.height, sprite.width,
            sprite.height, sprite_nonzero);

  const st::math::Rect cover = box.inset(st::math::Insets::all(1.0f));

  struct Run {
    std::string name;
    int which;   // 0 = 两层合成；1 = 贴图；2 = 贴图 + 跳过被盖区
  };
  const std::vector<Run> runs{{"A 现状：两层各自合成", 0},
                              {"B 合并贴图（一次贴）", 1},
                              {"C 合并贴图 + 跳过被盖区", 2}};

  double reference = 0.0;
  for (const Run& run : runs) {
    Buffer buffer(background);
    std::size_t touched = 0;
    double best = 1e9;
    for (int index = 0; index < repeat; ++index) {
      buffer.clear(background);
      const auto start = Clock::now();
      if (run.which == 0) {
        touched = composite_layer(buffer, env);
        touched += composite_layer(buffer, key);
      } else {
        touched = blit_sprite(buffer, sprite, run.which == 2, &cover);
      }
      best = std::min(best,
                      std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    if (!reference) reference = best;
    st::print("  {:<28} {:>8.4} ms · 提速 {:>5.2}× · 触碰 {:>6} px\n", run.name, best,
              reference / std::max(best, 1e-9), touched);
  }

  // 等价性：A 与 C 在**铺上不透明底色后**必须一致
  Buffer a_buffer(background);
  (void)composite_layer(a_buffer, env);
  (void)composite_layer(a_buffer, key);
  Buffer c_buffer(background);
  (void)blit_sprite(c_buffer, sprite, true, &cover);
  const Diff raw = diff_buffers(a_buffer, c_buffer);
  st::print("  A vs C（未铺底）：超差 {} px（最大Δ{}）\n", raw.differing, raw.max_delta);
  return 0;
}
