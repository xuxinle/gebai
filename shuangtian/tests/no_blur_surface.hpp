/// 不支持毛玻璃的画布桩：把 `Canvas` 的像素缓冲**包一层**，`blur_region` 恒 false、
/// `supports_blur` 恒假，其余原语全部转发给内层真 Canvas。
///
/// 为何不继承 `Canvas`：它是 `final`（自研光栅器不走虚表分发，子类化本来就
/// 不是它的扩展点；`Surface` 才是多态入口——GPU 腿 `GpuCanvas` 也是这样挂在
/// `Surface` 下的）。测试桩只需要“像一个不支持模糊的软件画布”，转发即可。
///
/// 转发的原语清单以 `Element::paint` 实际用到的为准（clear/fill_rect/push_clip/
/// pop_clip/像素读写）；`Panel` 无文本时不需要 text port。
#pragma once

#include "st/raster/canvas.hpp"

#include <cstdint>
#include <span>

namespace stdtest {

class NoBlurSurface final : public st::raster::Surface {
 public:
  explicit NoBlurSurface(int width, int height)
      : inner_(width, height, 1.0f) {}

  // —— 尺寸与坐标（Surface 纯虚；`logical_*` 基类已有内联实现，这里转发）——
  [[nodiscard]] auto physical_width() const noexcept -> int override {
    return inner_.physical_width();
  }
  [[nodiscard]] auto physical_height() const noexcept -> int override {
    return inner_.physical_height();
  }
  [[nodiscard]] auto device_scale() const noexcept -> float override {
    return inner_.device_scale();
  }
  void set_device_scale(float scale) override { inner_.set_device_scale(scale); }
  [[nodiscard]] auto to_physical(st::math::Rect rect) const noexcept -> st::math::IntRect override {
    return inner_.to_physical(rect);
  }
  [[nodiscard]] auto to_physical(st::math::Point point) const noexcept -> st::math::Point override {
    return inner_.to_physical(point);
  }

  // —— 剖析（桩不需要）——
  void set_profiler(st::raster::PaintProfiler* profiler) noexcept override {
    inner_.set_profiler(profiler);
  }
  [[nodiscard]] auto profiler() const noexcept -> st::raster::PaintProfiler* override {
    return inner_.profiler();
  }
  void add_profile(st::raster::PaintOp op, double ms, std::uint64_t pixels,
                   std::source_location site) noexcept override {
    inner_.add_profile(op, ms, pixels, site);
  }

  // —— 绘制原语（转发）——
  void clear(st::math::Color color) override { inner_.clear(color); }
  void fill_rect(st::math::Rect rect, const st::raster::Paint& paint, float radius = 0.0f,
                 st::raster::DrawOptions options = {}) override {
    inner_.fill_rect(rect, paint, radius, options);
  }
  void fill_path(const st::raster::Path& path, const st::raster::Paint& paint,
                 st::raster::DrawOptions options = {}) override {
    inner_.fill_path(path, paint, options);
  }
  void stroke_path(const st::raster::Path& path, const st::raster::Paint& paint, float width,
                   const st::raster::StrokeStyle& style = st::raster::StrokeStyle{},
                   st::raster::DrawOptions options = {}) override {
    inner_.stroke_path(path, paint, width, style, options);
  }
  void fill_circle(st::math::Point center, float radius, const st::raster::Paint& paint,
                   st::raster::DrawOptions options = {}) override {
    inner_.fill_circle(center, radius, paint, options);
  }
  void draw_shadow(st::math::Rect rect, float radius, float blur, st::math::Color color,
                   st::math::Point offset = {}, st::raster::DrawOptions options = {}) override {
    inner_.draw_shadow(rect, radius, blur, color, offset, options);
  }
  void draw_shadow_layered(st::math::Rect rect, float radius, st::math::Color key_color,
                           float key_blur, st::math::Point key_offset,
                           st::math::Color ambient_color, float ambient_blur,
                           st::math::Point ambient_offset,
                           st::raster::DrawOptions options = {}) override {
    inner_.draw_shadow_layered(rect, radius, key_color, key_blur, key_offset, ambient_color,
                               ambient_blur, ambient_offset, options);
  }
  void draw_canvas(const st::raster::Surface& source, st::math::Rect destination,
                   st::raster::DrawOptions options = {}) override {
    inner_.draw_canvas(source, destination, options);
  }
  void draw_canvas_at(const st::raster::Surface& source, int x, int y,
                      st::raster::DrawOptions options = {}) override {
    inner_.draw_canvas_at(source, x, y, options);
  }

  // —— 本桩存在的意义：不支持毛玻璃 ——
  [[nodiscard]] auto blur_region(st::math::Rect rect, float radius) -> bool override {
    (void)rect;
    (void)radius;
    return false;
  }
  [[nodiscard]] auto supports_blur() const noexcept -> bool override { return false; }

  // —— 覆盖率位图混合（转发）——
  void blend_coverage_bitmap(int x, int y, std::span<const float> coverage, int width, int height,
                             const st::raster::Paint& paint, float opacity,
                             st::raster::BlendMode blend, std::uint64_t cache_key = 0,
                             st::raster::CoverageFormat format =
                                 st::raster::CoverageFormat::Grayscale) override {
    inner_.blend_coverage_bitmap(x, y, coverage, width, height, paint, opacity, blend, cache_key,
                                 format);
  }
  void blend_coverage_row(int y, int x_begin, std::span<const float> coverage,
                          const st::raster::Paint& paint, float opacity,
                          st::raster::BlendMode blend) override {
    inner_.blend_coverage_row(y, x_begin, coverage, paint, opacity, blend);
  }
  void blend_coverage_row_lcd(int y, int x_begin, std::span<const float> coverage,
                              const st::raster::Paint& paint, float opacity,
                              st::raster::BlendMode blend) override {
    inner_.blend_coverage_row_lcd(y, x_begin, coverage, paint, opacity, blend);
  }

  // —— 裁剪（转发）——
  void push_clip_rect(st::math::Rect rect) override { inner_.push_clip_rect(rect); }
  void push_clip_rounded_rect(st::math::Rect rect, float radius) override {
    inner_.push_clip_rounded_rect(rect, radius);
  }
  void push_clip_path(const st::raster::Path& path) override { inner_.push_clip_path(path); }
  void pop_clip() override { inner_.pop_clip(); }
  [[nodiscard]] auto clip_depth() const noexcept -> std::size_t override {
    return inner_.clip_depth();
  }
  [[nodiscard]] auto clip_rect() const noexcept -> st::math::IntRect override {
    return inner_.clip_rect();
  }
  [[nodiscard]] auto has_mask_clip() const noexcept -> bool override {
    return inner_.has_mask_clip();
  }

  // —— 像素访问（转发）——
  [[nodiscard]] auto pixel_at(int x, int y) const -> st::math::Color override {
    return inner_.pixel_at(x, y);
  }
  void set_pixel(int x, int y, st::math::Color color) override { inner_.set_pixel(x, y, color); }
  [[nodiscard]] auto pixel_at_point(st::math::Point point) const -> st::math::Color override {
    return inner_.pixel_at_point(point);
  }
  [[nodiscard]] auto pixels() const noexcept -> std::span<const std::uint32_t> override {
    return inner_.pixels();
  }
  [[nodiscard]] auto pixels() noexcept -> std::span<std::uint32_t> override {
    return inner_.pixels();
  }
  [[nodiscard]] auto to_rgba8() const -> std::vector<std::uint8_t> override {
    return inner_.to_rgba8();
  }
  [[nodiscard]] auto content_bounds() const noexcept -> st::math::IntRect override {
    return inner_.content_bounds();
  }
  [[nodiscard]] auto content_bounds_logical() const noexcept -> st::math::IntRect override {
    return inner_.content_bounds_logical();
  }

 private:
  st::raster::Canvas inner_;
};

}  // namespace stdtest
