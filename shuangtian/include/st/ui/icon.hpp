#pragma once

/// 自绘矢量图标集：24×24 视图框 + 描边式路径数据（零位图资源，任意缩放清晰）。
/// 数据结构为 `inline constexpr` 表（`CONVENTIONS.md` §3.6 允许的数据表形态）。

#include <cstdint>
#include <string_view>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/path.hpp"

namespace st::ui {

struct IconGlyph {
  std::string_view name{};
  std::string_view data{};  ///< 路径数据：M/L/C/Z 指令（空格分隔的绝对坐标，24×24 视图框）
  float stroke{2.0f};       ///< 视图框单位下的描边宽度
  bool filled{false};       ///< true 时按填充绘制（实心图标）
};

class Icon {
 public:
  /// 全部图标名（字典序）。
  [[nodiscard]] static auto names() -> std::vector<std::string_view>;
  [[nodiscard]] static auto has(std::string_view name) noexcept -> bool;

  /// 构造缩放到 `box` 的路径（`stroke_width` 为画布像素宽，用于描边式图标）。
  [[nodiscard]] static auto path(std::string_view name, math::Rect box,
                                 float stroke_width = 2.0f) -> raster::Path;
  /// 按 `color` 绘制到画布（描边式；`stroke_width` 默认 2px）。
  static void draw(raster::Surface& canvas, std::string_view name, math::Rect box, math::Color color,
                   float stroke_width = 2.0f);
  /// 填充式绘制（`filled` 图标）。
  static void draw_filled(raster::Surface& canvas, std::string_view name, math::Rect box,
                          math::Color color);

  /// 图标在 24×24 视图框内的实际包围盒（布局对齐用）。
  [[nodiscard]] static auto view_bounds(std::string_view name) -> math::Rect;

 private:
  [[nodiscard]] static auto find(std::string_view name) noexcept -> const IconGlyph*;
};

}  // namespace st::ui
