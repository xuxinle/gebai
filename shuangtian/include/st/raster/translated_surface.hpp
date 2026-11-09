#pragma once

/// **平移绘制面**（`raster::TranslatedSurface`）：把一次绘制的全部坐标整体偏移。
///
/// ## 为什么不是「给每个原语加一个 `offset` 参数」
///
/// 动画里的位移（`translateY` / `slideIn` / `shake`）作用在**整棵子树**上：
/// 元素自己画的框体、它的文字、自绘形状、以及所有后代，都要一起走。
/// 而 `Surface` 有 **44 个虚函数**（两条渲染腿各实现一遍，共 ~30 处坐标换算点），
/// 逐原语加参数意味着：两条腿 × 每个原语 × 每处 `scale_` 换算全部要改，
/// 且任何一处漏改都是“位移之后有一类东西留在原地”——这种缺陷只能靠肉眼发现。
///
/// 本类把这件事收成**一个**位置：包一层委托，只在地理量上加减偏移。
/// 于是：
/// - **两条腿零改动**（它不碰 `Canvas` / `GpuCanvas` 的任何代码）；
/// - 未来新增后端口自动具备该能力（不需要谁记得实现它）；
/// - 判据变成一条可证的等价式（见下）。
///
/// ## 语义契约（三条）
///
/// 1. **绘制坐标整体平移**：`fill_rect({x,y,…})` 落到 `inner` 的 `{x+dx, y+dy, …}`；
///    `blend_coverage_bitmap` 的物理坐标按 `offset * device_scale` 平移；
///    `push_clip_path` 的路径（**物理像素**）同样平移。
/// 2. **查询坐标反向补偿**：`clip_rect()` 返回**减去偏移后**的裁剪域，
///    因为调用方（元素的视口剔除）拿它和**未平移的** `bounds_` 比。
///    若这里返回平移后的域，位移元素的后代会被错误剔除（“滑入的元素里有一半不显示”）。
/// 3. **`clear()` 不清屏**：整屏清屏是帧级操作，属于宿主语义（`inner` 的 `clear` 才是）。
///    这里显式转发给 `inner`——因为本包装只在**元素子树**范围内存在，
///    真的调它说明调用方本来就想清屏。
///
/// 把“平移”做成**包装**而不是状态还带来一个好处：作用域由 C++ 的栈自然界定——
/// `{ TranslatedSurface shifted(canvas, dx, dy); element.paint(context, shifted); }`
/// 出了这对大括号就归位，不存在“忘了弹出偏移”的悬垂态
/// （这正是 `push_clip` 漏 `pop` 那类缺陷的根）。

#include <cmath>
#include <span>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/raster/surface.hpp"

namespace st::raster {

class TranslatedSurface final : public Surface {
 public:
  /// `offset` 是**逻辑像素**（与 UI 坐标同口径）。
  TranslatedSurface(Surface& inner, math::Point offset) noexcept
      : inner_(inner), offset_(offset) {}

  [[nodiscard]] auto inner() noexcept -> Surface& { return inner_; }
  [[nodiscard]] auto offset() const noexcept -> math::Point { return offset_; }

  // —— 尺寸与坐标换算 ——
  [[nodiscard]] auto physical_width() const noexcept -> int override {
    return inner_.physical_width();
  }
  [[nodiscard]] auto physical_height() const noexcept -> int override {
    return inner_.physical_height();
  }
  [[nodiscard]] auto device_scale() const noexcept -> float override {
    return inner_.device_scale();
  }
  [[nodiscard]] auto supports_partial_repaint() const noexcept -> bool override {
    return inner_.supports_partial_repaint();
  }
  void set_device_scale(float scale) override { inner_.set_device_scale(scale); }
  [[nodiscard]] auto resize(int physical_width, int physical_height) -> Status override {
    return inner_.resize(physical_width, physical_height);
  }
  /// **不做平移**：这是“逻辑 → 物理”的纯换算，平移已在各原语里加过。
  /// （在此再平移会让 `to_physical` 的调用方双重偏移。）
  [[nodiscard]] auto to_physical(math::Rect rect) const noexcept -> math::IntRect override {
    return inner_.to_physical(rect);
  }
  [[nodiscard]] auto to_physical(math::Point point) const noexcept -> math::Point override {
    return inner_.to_physical(point);
  }

  // —— 剖析 ——
  void set_profiler(PaintProfiler* profiler) noexcept override { inner_.set_profiler(profiler); }
  [[nodiscard]] auto profiler() const noexcept -> PaintProfiler* override {
    return inner_.profiler();
  }
  void add_profile(PaintOp op, double ms, std::uint64_t pixels = 0,
                   const std::source_location site = std::source_location::current()) noexcept override {
    inner_.add_profile(op, ms, pixels, site);
  }

  // —— 绘制原语（几何平移） ——
  void clear(math::Color color) override { inner_.clear(color); }
  // 覆写**不继承默认实参**（C++ 规则），所以每个覆写都要重写一遍默认值——
  // 否则调用方写 `fill_rect(rect, paint)` 会“找不到匹配的重载”。
  void fill_rect(math::Rect rect, const Paint& paint, float radius = 0.0f,
                 DrawOptions options = {}) override {
    inner_.fill_rect(rect.offset(offset_.x, offset_.y), paint, radius, options);
  }
  void fill_path(const Path& path, const Paint& paint, DrawOptions options = {}) override {
    inner_.fill_path(path.translated(offset_.x * scale(), offset_.y * scale()), paint, options);
  }
  void stroke_path(const Path& path, const Paint& paint, float width,
                   const StrokeStyle& style = StrokeStyle{},
                   DrawOptions options = {}) override {
    inner_.stroke_path(path.translated(offset_.x * scale(), offset_.y * scale()), paint, width,
                       style, options);
  }
  void fill_circle(math::Point center, float radius, const Paint& paint,
                   DrawOptions options = {}) override {
    inner_.fill_circle(math::Point{center.x + offset_.x, center.y + offset_.y}, radius, paint,
                       options);
  }
  void draw_shadow(math::Rect rect, float radius, float blur, math::Color color,
                   math::Point shadow_offset = {}, DrawOptions options = {}) override {
    inner_.draw_shadow(rect.offset(offset_.x, offset_.y), radius, blur, color, shadow_offset,
                       options);
  }
  void draw_shadow_layered(math::Rect rect, float radius, math::Color key_color, float key_blur,
                           math::Point key_offset, math::Color ambient_color,
                           float ambient_blur, math::Point ambient_offset,
                           DrawOptions options = {}) override {
    inner_.draw_shadow_layered(rect.offset(offset_.x, offset_.y), radius, key_color, key_blur,
                               key_offset, ambient_color, ambient_blur, ambient_offset, options);
  }
  void draw_canvas(const Surface& source, math::Rect destination,
                   DrawOptions options = {}) override {
    inner_.draw_canvas(source, destination.offset(offset_.x, offset_.y), options);
  }
  void draw_canvas_at(const Surface& source, int x, int y, DrawOptions options = {}) override {
    const auto dx = static_cast<int>(std::lround(offset_.x * scale()));
    const auto dy = static_cast<int>(std::lround(offset_.y * scale()));
    inner_.draw_canvas_at(source, x + dx, y + dy, options);
  }
  [[nodiscard]] auto blur_region(math::Rect rect, float radius) -> bool override {
    return inner_.blur_region(rect.offset(offset_.x, offset_.y), radius);
  }
  [[nodiscard]] auto supports_blur() const noexcept -> bool override {
    return inner_.supports_blur();
  }
  /// **纹理句柄透传**（不是本包装画的，是同后端的 `inner`）：零拷贝快路的语义是
  /// “采样这张纹理”，与坐标平移无关，透传即可。
  [[nodiscard]] auto native_texture() const noexcept -> NativeTexture override {
    return inner_.native_texture();
  }
  [[nodiscard]] auto composite_native_texture(const NativeTexture& texture,
                                              math::Rect destination,
                                              DrawOptions options = {}) -> bool override {
    return inner_.composite_native_texture(texture, destination.offset(offset_.x, offset_.y),
                                           options);
  }
  /// 覆盖率位图的坐标是**物理像素**（`text` 按 `origin * device_scale` 算好后传进来），
  /// 所以平移量也要先乘 `device_scale`——这是全类唯一需要动手算的地方。
  void blend_coverage_bitmap(int x, int y, std::span<const float> coverage, int width, int height,
                             const Paint& paint, float opacity, BlendMode blend,
                             std::uint64_t cache_key = 0,
                             CoverageFormat format = CoverageFormat::Grayscale) override {
    inner_.blend_coverage_bitmap(x + physical_dx(), y + physical_dy(), coverage, width, height,
                                 paint, opacity, blend, cache_key, format);
  }
  void blend_coverage_row(int y, int x_begin, std::span<const float> coverage, const Paint& paint,
                          float opacity, BlendMode blend) override {
    inner_.blend_coverage_row(y + physical_dy(), x_begin + physical_dx(), coverage, paint, opacity,
                              blend);
  }
  void blend_coverage_row_lcd(int y, int x_begin, std::span<const float> coverage,
                              const Paint& paint, float opacity, BlendMode blend) override {
    inner_.blend_coverage_row_lcd(y + physical_dy(), x_begin + physical_dx(), coverage, paint,
                                  opacity, blend);
  }

  // —— 裁剪（推入时平移；查询时反向补偿，见文件头契约第 2 条） ——
  void push_clip_rect(math::Rect rect) override {
    inner_.push_clip_rect(rect.offset(offset_.x, offset_.y));
  }
  void push_clip_rounded_rect(math::Rect rect, float radius) override {
    inner_.push_clip_rounded_rect(rect.offset(offset_.x, offset_.y), radius);
  }
  void push_clip_path(const Path& path) override {
    inner_.push_clip_path(path.translated(offset_.x * scale(), offset_.y * scale()));
  }
  void pop_clip() override { inner_.pop_clip(); }
  [[nodiscard]] auto clip_depth() const noexcept -> std::size_t override {
    return inner_.clip_depth();
  }
  [[nodiscard]] auto clip_rect() const noexcept -> math::IntRect override {
    const math::IntRect physical = inner_.clip_rect();
    // 反向补偿：调用方拿它和**未平移**的元素坐标比。
    return math::IntRect{physical.x - physical_dx(), physical.y - physical_dy(),
                         physical.width, physical.height};
  }
  [[nodiscard]] auto has_mask_clip() const noexcept -> bool override {
    return inner_.has_mask_clip();
  }

  // —— 像素访问（物理坐标：平移） ——
  [[nodiscard]] auto pixel_at(int x, int y) const -> math::Color override {
    return inner_.pixel_at(x + physical_dx(), y + physical_dy());
  }
  void set_pixel(int x, int y, math::Color color) override {
    inner_.set_pixel(x + physical_dx(), y + physical_dy(), color);
  }
  [[nodiscard]] auto pixel_at_point(math::Point point) const -> math::Color override {
    return inner_.pixel_at_point(
        math::Point{point.x + offset_.x, point.y + offset_.y});
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
  void skip_next_packed_readback() noexcept override { inner_.skip_next_packed_readback(); }
  [[nodiscard]] auto content_bounds() const noexcept -> math::IntRect override {
    return inner_.content_bounds();
  }
  [[nodiscard]] auto content_bounds_logical() const noexcept -> math::IntRect override {
    return inner_.content_bounds_logical();
  }

 private:
  [[nodiscard]] auto scale() const noexcept -> float { return inner_.device_scale(); }
  [[nodiscard]] auto physical_dx() const noexcept -> int {
    return static_cast<int>(std::lround(offset_.x * scale()));
  }
  [[nodiscard]] auto physical_dy() const noexcept -> int {
    return static_cast<int>(std::lround(offset_.y * scale()));
  }

  Surface& inner_;
  math::Point offset_{};
};

}  // namespace st::raster
