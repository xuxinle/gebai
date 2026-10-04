/// **`max_shift` 参数在 UI 小字上的适配性**探针（仅验证用）。
///
/// 假说：`grid_fit` 的护栏 `max_shift`（单边最大位移 0.5 物理像素）在 UI 字号上过紧。
///
/// 推理链：
///   1. 宽度量化把笔画宽度取整（`round(width_px)`）。UI 拉丁/CJK 的主干普遍是
///      1.5~1.7 物理像素，取整成 2 就要求**远边额外移动 0.3~0.5 像素**；
///   2. 代码对两侧**各自**做 `|delta| <= max_shift` 判定：近边（0.1px）通过并被应用，
///      远边（0.6px）被拒 —— 结果是笔画被**平移了但没被改宽**，反而更不容易对齐网格，
///      还把字形推歪了一点；
///   3. 字号越大宽度量化的相对位移越小（21px 时主干 ~2.8px，取整到 3 只差 0.2px），
///      所以这个缺陷只在**小字**上显形——恰好就是「UI 字体差点意思」的字号区间。
///
/// 做法：复刻 `TextRenderer` 的位图管线，只换 `max_shift` 取值，逐档量
/// `half`（0.45~0.55 的半覆盖像素 —— 糊边的直接证据）与 `ink`（墨量，看有没有被抽瘦）。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/font.hpp"
#include "st/text/grid_fit.hpp"
#include "st/text/text.hpp"

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::text::FontFace;
using st::text::FontStack;
using st::text::GridFitMode;
using st::text::GridFitOptions;
using st::text::GridFitResult;

namespace {

constexpr int kSubpixelColumns = 3;

struct Stat {
  double ink{0.0};
  int solid{0};
  int mid{0};
  int half{0};
  int applied{0};
  int stems{0};
};

/// 复刻 `TextRenderer::glyph_bitmap` 的灰度分支，把 `max_shift` 作为参数暴露。
[[nodiscard]] auto render_glyph(const FontFace& face, std::uint32_t glyph, float pixel_size,
                                float supersample, float max_shift) -> Stat {
  Stat stat;
  const auto outline = face.glyph_outline(glyph);
  if (!outline.has_value() || outline->is_empty()) return stat;
  const auto& metrics = face.metrics();
  const float units = metrics.units_per_em > 0.0f ? metrics.units_per_em : 1000.0f;
  const int ss = std::max(1, static_cast<int>(std::lround(supersample)));
  const float scale = pixel_size * static_cast<float>(ss) / units;

  const auto build = [&outline, scale](float horizontal) {
    st::raster::Path path;
    const auto map = [scale, horizontal](Point p) noexcept -> Point {
      return Point{p.x * scale * horizontal, -p.y * scale};
    };
    for (const auto& command : outline->commands()) {
      switch (command.kind) {
        case st::raster::PathCommand::Kind::MoveTo: path.move_to(map(command.p1)); break;
        case st::raster::PathCommand::Kind::LineTo: path.line_to(map(command.p1)); break;
        case st::raster::PathCommand::Kind::QuadTo:
          path.quad_to(map(command.p1), map(command.p2));
          break;
        case st::raster::PathCommand::Kind::CubicTo:
          path.cubic_to(map(command.p1), map(command.p2), map(command.p3));
          break;
        case st::raster::PathCommand::Kind::Close: path.close(); break;
      }
    }
    return path;
  };
  const st::raster::Path transformed = build(1.0f);
  const auto bounds = transformed.flattened_bounds(0.2f);
  const int pad = 1 * ss;
  const auto align_down = [ss](int v) {
    const int r = v % ss;
    return r == 0 ? v : v - (r < 0 ? r + ss : r);
  };
  const auto align_up = [&align_down, ss](int v) {
    const int d = align_down(v);
    return d == v ? v : d + ss;
  };
  const int min_x = align_down(static_cast<int>(std::floor(bounds.x)) - pad);
  const int min_y = align_down(static_cast<int>(std::floor(bounds.y)) - pad);
  const int max_x = align_up(static_cast<int>(std::ceil(bounds.right())) + pad);
  const int max_y = align_up(static_cast<int>(std::ceil(bounds.bottom())) + pad);
  const int width = max_x - min_x;
  const int height = max_y - min_y;
  if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return stat;

  const auto local = transformed.translated(static_cast<float>(-min_x), static_cast<float>(-min_y));
  GridFitOptions options;
  options.mode = GridFitMode::Normal;
  options.grid = static_cast<float>(ss);
  options.max_shift = max_shift;
  const GridFitResult fitted = st::text::grid_fit(local, options);
  stat.applied = fitted.applied ? 1 : 0;
  stat.stems = fitted.vertical_stems + fitted.horizontal_stems;

  Canvas scratch{width, height};
  scratch.fill_path(fitted.path, st::raster::Paint::solid(Color::rgb(255, 255, 255)));

  const int out_w = std::max(1, width / ss);
  const int out_h = std::max(1, height / ss);
  for (int y = 0; y < out_h; ++y) {
    for (int x = 0; x < out_w; ++x) {
      float total = 0.0f;
      for (int sy = 0; sy < ss; ++sy) {
        for (int sx = 0; sx < ss; ++sx) {
          total += static_cast<float>(scratch.pixel_at(x * ss + sx, y * ss + sy).a) / 255.0f;
        }
      }
      const float v = total / static_cast<float>(ss * ss);
      stat.ink += v;
      if (v > 0.85f) ++stat.solid;
      else if (v > 0.15f) ++stat.mid;
      if (v > 0.45f && v < 0.55f) ++stat.half;
    }
  }
  return stat;
}

}  // namespace

auto main() -> int {
  auto stack = FontStack::system_default();
  if (!stack) return 1;
  const FontStack& fonts = *stack;
  const float device_scale = 1.5f;
  const int ss = static_cast<int>(std::lround(device_scale));

  constexpr std::string_view kCjk = "资源管理器设置概览数据控制通道关于按钮";
  constexpr std::string_view kLatin = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP";
  st::print("device_scale={} supersample={}（应用实际口径）\n", device_scale, ss);
  st::print("「half」= 0.45~0.55 半覆盖像素（糊边）；「applied」= 该字形拟合是否生效\n\n");

  for (const auto [text, tag] : {std::pair{kCjk, "CJK"}, std::pair{kLatin, "拉丁"}}) {
    st::print("=== {} ===\n", tag);
    st::print("  {:>5s} {:>7s}", "逻辑px", "max_shift");
    st::print(" {:>7s} {:>7s} {:>7s} {:>8s} {:>7s}\n", "applied", "half", "mid", "mid/solid",
              "ink");
    for (const float size : {12.0f, 13.0f, 13.5f, 16.0f, 21.0f}) {
      const float pixel_size = size * device_scale;
      for (const float max_shift : {0.5f, 1.0f, 1.5f, 3.0f}) {
        Stat total;
        int glyphs = 0;
        for (const char32_t codepoint : st::utf8_decode(text)) {
          const auto* face = fonts.find_face(codepoint);
          if (face == nullptr) continue;
          const auto id = face->glyph_index(codepoint);
          if (!id.has_value()) continue;
          const Stat one =
              render_glyph(*face, *id, pixel_size, device_scale, max_shift);
          total.ink += one.ink;
          total.solid += one.solid;
          total.mid += one.mid;
          total.half += one.half;
          total.applied += one.applied;
          ++glyphs;
        }
        st::print("  {:>5.1f} {:>9.1f}", size, max_shift);
        st::print(" {:>7d} {:>7d} {:>7d} {:>8.3f} {:>7.0f}  ({}/{} 字形拟合生效)\n", total.applied,
                  total.half, total.mid,
                  total.solid > 0 ? static_cast<double>(total.mid) / total.solid : -1.0, total.ink,
                  total.applied, glyphs);
      }
      st::print("\n");
    }
  }
  return 0;
}
