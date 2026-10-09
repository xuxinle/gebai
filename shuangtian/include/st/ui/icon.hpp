#pragma once

/// 自绘矢量图标集：24×24 视图框 + 描边式路径数据（零位图资源，任意缩放清晰）。
/// 数据结构为 `inline constexpr` 表（`CONVENTIONS.md` §3.6 允许的数据表形态）。

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/path.hpp"
#include "st/ui/svg.hpp"

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

  /// 图标画在 `box` 里时的**实际墨迹尺寸**（逻辑 px）。
  ///
  /// 为何需要：`path` 会把墨迹等比缩放**居中**到 `box`，于是墨迹通常**小于 box**
  /// （归一化后的留白）。宿主若按 box 尺寸排版，那些透明留白也占位置——
  /// 实测（`Button` 图标+文字）：内容重心偏移恰为 `(box − 墨迹) / 4`，
  /// 且随 box 变大而线性变大。要“看着居中”就得按墨迹排版。
  ///
  /// 与 `path` **同一算式**：几何 × fit 再补上描边外扩（描边式两侧各半个线宽，
  /// 实心式无外扩）。两者必须是同一个事实，否则“占多宽”与“画多宽”会再次脱钩。
  [[nodiscard]] static auto ink_size(std::string_view name, math::Size box) -> math::Size;

 private:
  [[nodiscard]] static auto find(std::string_view name) noexcept -> const IconGlyph*;
};

/// SVG 图标注册表：进程级共享一份 IconSet + 每调用点一份位图缓存。
///
/// 用法：应用启动时 `svg_registry().load(sprite_text)` 装载一份 sprite（或逐个
/// `add_single`）；此后任何 `IconView`/`Icon::draw` 遇到**表里没有的图标名**都会先查这里。
/// 图标名带 `svg:` 前缀（如 `svg:folder`）则强制走 SVG 源。矢量按目标尺寸重新光栅化
/// （任意缩放清晰），位图按 (id, 物理尺寸, 颜色) LRU 缓存（同尺寸零重栅）。
///
/// `registry()` 返回的共享实例由首次调用惰性构造（ Meyers singleton，进程生存期、
/// 无退出期析构顺序问题——只持有不可变数据与缓存位图）。
class SvgIconRegistry {
 public:
  /// 装载 sprite（`<symbol id>` 形态；已有同名 id 会被覆盖语义：追加新图标为主）。
  auto load_sprite(std::string_view source) -> bool;
  /// 追加单图标（`<svg>` 根形态）。
  auto add_single(std::string_view id, std::string_view source) -> bool;
  [[nodiscard]] auto has(std::string_view id) const -> bool;
  [[nodiscard]] auto ids() const -> std::vector<std::string>;
  /// 绘制（带位图缓存）；返回 false = id 不存在或尺寸非法。
  auto draw(raster::Surface& canvas, std::string_view id, math::Rect box,
            math::Color color) const -> bool;
  /// 丢弃全部位图缓存（矢量数据保留；主题切换不需要——颜色在缓存键里）。
  void clear_cache() const;
  [[nodiscard]] auto cache_entries() const -> std::size_t;

 private:
  SvgIconRegistry() = default;
  friend auto svg_registry() -> SvgIconRegistry&;

  std::shared_ptr<svg::IconSet> set_{std::make_shared<svg::IconSet>()};
  mutable svg::IconSetPainter painter_{set_};
};

/// 进程级 SVG 图标注册表（见 `SvgIconRegistry`）。
auto svg_registry() -> SvgIconRegistry&;

}  // namespace st::ui
