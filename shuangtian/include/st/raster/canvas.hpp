#pragma once

/// 离屏画布：预乘 RGBA8 像素缓冲 + 全自绘绘制原语 + 裁剪栈。
/// 设计要点（`DESIGN.md` §4.2）：
/// - 软件光栅器是唯一真相源（无 GPU 也 100% 可用）；
/// - 抗锯齿为**扫描线覆盖率**（非 MSAA），1px 发丝边框在任何 DPI 下平滑；
/// - 裁剪：矩形裁剪走快速路径（区间交集），圆角/路径裁剪走 8 位遮罩；
/// - 混合在预乘空间做 16 位中间运算，避免 8 位往返误差。

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace st::raster {

/// 8 位覆盖率遮罩（圆角/路径裁剪、阴影模糊）。
class Mask {
 public:
  Mask(int width, int height);

  [[nodiscard]] auto width() const noexcept -> int { return width_; }
  [[nodiscard]] auto height() const noexcept -> int { return height_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return width_ <= 0 || height_ <= 0; }
  [[nodiscard]] auto at(int x, int y) const noexcept -> std::uint8_t;
  void set(int x, int y, std::uint8_t value) noexcept;
  [[nodiscard]] auto values() noexcept -> std::span<std::uint8_t>;
  [[nodiscard]] auto values() const noexcept -> std::span<const std::uint8_t>;
  /// 覆盖率抽样（双线性，坐标为遮罩局部坐标）。
  [[nodiscard]] auto sample(float x, float y) const noexcept -> float;

 private:
  int width_{0};
  int height_{0};
  std::vector<std::uint8_t> values_{};
};

class Canvas {
 public:
  /// 直接指定**物理**缓冲尺寸（`device_scale` 默认 1.0，此时逻辑坐标 = 物理像素）。
  Canvas(int physical_width, int physical_height, float device_scale = 1.0f);
  /// 按**逻辑**尺寸与 DPI 缩放建画布：物理缓冲 = round(logical × scale)。
  [[nodiscard]] static auto for_logical_size(int logical_width, int logical_height,
                                             float device_scale) -> Canvas;
  ~Canvas();
  Canvas(const Canvas&) = delete;
  auto operator=(const Canvas&) -> Canvas& = delete;
  Canvas(Canvas&& other) noexcept;
  auto operator=(Canvas&& other) noexcept -> Canvas&;

  /// 逻辑尺寸（UI 视角；= 物理尺寸 / device_scale）。
  [[nodiscard]] auto width() const noexcept -> int {
    return static_cast<int>(std::lround(static_cast<float>(physical_width_) / scale_));
  }
  [[nodiscard]] auto height() const noexcept -> int {
    return static_cast<int>(std::lround(static_cast<float>(physical_height_) / scale_));
  }
  /// 物理缓冲尺寸（像素存取/编码视角）。
  [[nodiscard]] auto physical_width() const noexcept -> int { return physical_width_; }
  [[nodiscard]] auto physical_height() const noexcept -> int { return physical_height_; }
  /// DPI 缩放（物理像素 / 逻辑像素）。
  [[nodiscard]] auto device_scale() const noexcept -> float { return scale_; }
  void set_device_scale(float scale) noexcept;
  [[nodiscard]] auto logical_bounds() const noexcept -> math::Rect {
    return math::Rect{0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height())};
  }
  [[nodiscard]] auto bounds() const noexcept -> math::Rect { return logical_bounds(); }
  [[nodiscard]] auto int_bounds() const noexcept -> math::IntRect {
    return math::IntRect{0, 0, width(), height()};
  }
  [[nodiscard]] auto physical_bounds() const noexcept -> math::IntRect {
    return math::IntRect{0, 0, physical_width_, physical_height_};
  }
  /// 逻辑矩形 → 物理整数矩形（向外取整，覆盖完整像素）。
  [[nodiscard]] auto to_physical(math::Rect rect) const noexcept -> math::IntRect;
  [[nodiscard]] auto to_physical(math::Point point) const noexcept -> math::Point;
  /// 逻辑尺寸 → 物理像素数。
  [[nodiscard]] auto scale_length(float logical) const noexcept -> float { return logical * scale_; }

  void clear(math::Color color);

  // —— 绘制原语 ——
  void fill_rect(math::Rect rect, const Paint& paint, float radius = 0.0f,
                 DrawOptions options = {});
  void fill_path(const Path& path, const Paint& paint, DrawOptions options = {});
  /// 描边（圆头圆角连接；宽度为总宽）。
  void stroke_path(const Path& path, const Paint& paint, float width, DrawOptions options = {});
  void fill_circle(math::Point center, float radius, const Paint& paint, DrawOptions options = {});
  /// 投影（半径 `blur` 的近似高斯模糊，偏移 `offset`）。
  void draw_shadow(math::Rect rect, float radius, float blur, math::Color color,
                   math::Point offset = {}, DrawOptions options = {});
  /// 位图合成（双线性缩放）。
  void draw_canvas(const Canvas& source, math::Rect destination, DrawOptions options = {});
  void draw_canvas_at(const Canvas& source, int x, int y, DrawOptions options = {});

  // —— 裁剪 ——
  /// 裁剪（逻辑坐标入参；内部按 `device_scale` 换算到物理像素）。
  void push_clip_rect(math::Rect rect);
  /// 圆角矩形裁剪（**逻辑坐标**；半径同样按 DPI 缩放）。
  void push_clip_rounded_rect(math::Rect rect, float radius);
  /// 路径裁剪（**路径按物理像素解释**——与 raster 层其余 API 一致；
  /// 持有逻辑坐标路径时先 `path.scaled(canvas.device_scale())`）。
  void push_clip_path(const Path& path);
  void pop_clip();
  [[nodiscard]] auto clip_rect() const noexcept -> math::IntRect;
  [[nodiscard]] auto has_mask_clip() const noexcept -> bool;

  // —— 像素访问（物理像素；逻辑坐标访问用 pixel_at_point）——
  [[nodiscard]] auto pixel_at(int x, int y) const -> math::Color;
  void set_pixel(int x, int y, math::Color color);
  /// 逻辑坐标取色（控制通道/测试用）。
  [[nodiscard]] auto pixel_at_point(math::Point point) const -> math::Color;
  [[nodiscard]] auto pixels() const noexcept -> std::span<const std::uint32_t> { return pixels_; }
  [[nodiscard]] auto pixels() noexcept -> std::span<std::uint32_t> { return pixels_; }
  /// 导出为直通 RGBA8 字节流（PNG 编码/截图用；物理分辨率）。
  [[nodiscard]] auto to_rgba8() const -> std::vector<std::uint8_t>;
  /// 非全透明像素的最小包围盒（**物理**像素；测试与截图裁剪用）。
  [[nodiscard]] auto content_bounds() const noexcept -> math::IntRect;
  /// 同上，但换算到逻辑像素。
  [[nodiscard]] auto content_bounds_logical() const noexcept -> math::IntRect;

  /// 低层：以覆盖率调制画笔颜色混合一行（栅格化器与自定义绘制使用）。
  /// `coverage[0]` 对应该行 `x_begin` 像素。
  void blend_coverage_row(int y, int x_begin, std::span<const float> coverage, const Paint& paint,
                          float opacity, BlendMode blend);

 private:
  struct ClipFrame {
    math::IntRect rect{};  ///< 物理像素
    std::shared_ptr<const Mask> mask{};  ///< 空表示仅矩形裁剪
    int mask_origin_x{0};
    int mask_origin_y{0};
  };

  [[nodiscard]] auto effective_alpha(int x, int y, float coverage) const noexcept -> float;
  void blend_span(int y, int x_begin, int x_end, math::Color color, float opacity, BlendMode mode);
  [[nodiscard]] auto current_clip() const noexcept -> const ClipFrame&;
  /// 物理像素坐标 → 逻辑坐标（画笔/渐变采样用）。
  [[nodiscard]] auto to_logical_point(float physical_x, float physical_y) const noexcept
      -> math::Point;

  int physical_width_{0};
  int physical_height_{0};
  float scale_{1.0f};
  float inverse_scale_{1.0f};
  std::vector<std::uint32_t> pixels_{};
  std::vector<ClipFrame> clip_stack_{};
};

/// 盒式模糊 ×3 近似高斯（阴影/毛玻璃用）；就地修改遮罩。
void blur_mask(Mask& mask, float radius);

}  // namespace st::raster
